#include "pch.h"

#include "resolver_probe.h"

#include "memory_range.h"
#include "signature_scan.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <utility>

#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif

namespace ds2::modding {
namespace {

constexpr std::uintptr_t kExpectedStreamReadRva = 0x000ABDC0;
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
constexpr std::uintptr_t kExpectedResourceIdentityRva = 0x009FEE5C;
constexpr std::uintptr_t kExpectedResourceIdentityOuterCallsiteRva = 0x009FA964;
constexpr std::uintptr_t kExpectedResourceIdentityOuterReturnRva = 0x009FA980;
constexpr std::uintptr_t kExpectedResourceReadCallsiteRva = 0x009FF0BF;
constexpr std::uintptr_t kExpectedResourceReadReturnRva = 0x009FF0D2;
#endif
constexpr std::uint64_t kMediaSegmentStart = 0x33250800ull;
constexpr std::uint64_t kMediaSegmentEnd = 0x332A3000ull;
constexpr std::size_t kEventSlotCount = 64;
constexpr std::size_t kTrackedMediaHandleCount = 4;
constexpr std::size_t kTrackedNonMediaHandleCount = 64;
#if defined(DS2_GENERAL_DDS_ENABLED)
constexpr std::size_t kGeneralRuntimeHashChunkSize = 16 * 1024;
constexpr std::size_t kMaxGeneralRuntimeHashBytes = 128 * 1024 * 1024;
#endif

using StreamReadFn = std::int32_t (*)(
    void* stream,
    void* destination,
    std::int32_t byte_count);
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
using ResourceIdentityFn = std::int32_t (*)(
    void* owner,
    void* argument2,
    void* argument3,
    void* argument4,
    void* argument5);
#endif
using ReadFileFn = decltype(&ReadFile);
using CloseHandleFn = decltype(&CloseHandle);

enum class SlotState : std::uint8_t {
    free,
    writing,
    ready,
    reading,
};

struct EventSlot final {
    std::atomic<SlotState> state{SlotState::free};
    ResolverProbeEvent event;
};

std::mutex g_lifecycle_mutex;
std::atomic<ResolverProbeState> g_state{ResolverProbeState::not_initialized};
std::atomic<bool> g_read_file_installed{};
std::atomic<bool> g_close_handle_installed{};
std::atomic<bool> g_stream_read_installed{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
std::atomic<bool> g_resource_identity_installed{};
#endif
std::atomic<int> g_last_minhook_status{static_cast<int>(MH_OK)};
std::atomic<ReadFileFn> g_original_read_file{};
std::atomic<CloseHandleFn> g_original_close_handle{};
std::atomic<StreamReadFn> g_original_stream_read{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
std::atomic<ResourceIdentityFn> g_original_resource_identity{};
#endif
void* g_read_file_target{};
void* g_close_handle_target{};
void* g_stream_read_target{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
void* g_resource_identity_target{};
std::uintptr_t g_game_image_base{};
#endif

std::atomic<ResolverProbeEventCallback> g_event_callback{};
std::atomic<bool> g_reported_stream_first_entry{};
std::array<EventSlot, kEventSlotCount> g_event_slots{};
std::atomic<std::size_t> g_next_write_slot{};
std::atomic<std::size_t> g_next_read_slot{};
std::array<std::atomic<HANDLE>, kTrackedMediaHandleCount> g_media_handles{};
std::array<std::atomic<HANDLE>, kTrackedNonMediaHandleCount> g_non_media_handles{};
FILE_ID_INFO g_target_file_id{};
std::atomic<bool> g_writer_enabled{};
std::atomic<bool> g_writer_faulted{};
std::atomic_flag g_writer_busy = ATOMIC_FLAG_INIT;
#if defined(DS2_GENERAL_DDS_ENABLED)
std::shared_ptr<const GeneralDdsCandidateSnapshot> g_general_candidates_owner;
std::atomic<const GeneralDdsCandidateSnapshot*> g_general_candidates{};
#endif

std::atomic<std::uint64_t> g_read_file_calls{};
std::atomic<std::uint64_t> g_package_segment_reads{};
std::atomic<std::uint64_t> g_target_sized_stream_reads{};
std::atomic<std::uint64_t> g_queued_events{};
std::atomic<std::uint64_t> g_dropped_events{};
std::atomic<std::uint64_t> g_sample_copy_failures{};
std::atomic<std::uint64_t> g_target_hash_matches{};
std::atomic<std::uint64_t> g_replacements_applied{};
std::atomic<std::uint64_t> g_replacement_write_failures{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
std::atomic<std::uint64_t> g_resource_identity_scopes{};
std::atomic<std::uint64_t> g_resource_identity_samples{};
std::atomic<std::uint64_t> g_resource_identity_overflows{};
std::atomic<std::uint64_t> g_resource_identity_context_resets{};

std::atomic<std::uint64_t> g_resource_identity_sequence{};
constinit thread_local ResourceIdentityThreadContext g_resource_identity_context;
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
std::atomic<std::uint64_t> g_general_candidate_hits{};
std::atomic<std::uint64_t> g_general_would_override{};
std::atomic<std::uint64_t> g_general_contract_mismatches{};
std::atomic<std::uint64_t> g_general_source_hash_failures{};
std::atomic<std::uint64_t> g_general_source_hash_mismatches{};
std::atomic<std::uint64_t> g_general_write_attempts{};
std::atomic<std::uint64_t> g_general_writes_completed{};
std::atomic<std::uint64_t> g_general_write_failures{};
std::atomic<std::uint64_t> g_general_write_verify_failures{};
#endif

void Notify(const ResolverProbeEventKind event) noexcept {
    const auto callback = g_event_callback.load(std::memory_order_acquire);
    if (callback != nullptr) {
        callback(event);
    }
}

void NotifyStreamFirstEntryOnce() noexcept {
    bool expected = false;
    if (g_reported_stream_first_entry.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        Notify(ResolverProbeEventKind::stream_first_entry);
    }
}

[[nodiscard]] EventSlot* ClaimEventSlot() noexcept {
    const std::size_t start = g_next_write_slot.fetch_add(
        1, std::memory_order_relaxed) % kEventSlotCount;
    for (std::size_t offset = 0; offset < kEventSlotCount; ++offset) {
        EventSlot& slot = g_event_slots[(start + offset) % kEventSlotCount];
        SlotState expected = SlotState::free;
        if (slot.state.compare_exchange_strong(
                expected,
                SlotState::writing,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return &slot;
        }
    }
    g_dropped_events.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void PublishEventSlot(
    EventSlot& slot,
    const ResolverProbeEventKind event_kind) noexcept {
    slot.state.store(SlotState::ready, std::memory_order_release);
    g_queued_events.fetch_add(1, std::memory_order_relaxed);
    Notify(event_kind);
}

void CaptureEventContext(ResolverProbeEvent& event) noexcept {
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) != FALSE) {
        event.performance_counter = counter.QuadPart;
    }
    event.thread_id = GetCurrentThreadId();
    std::array<void*, kResolverProbeMaxFrames> frames{};
    event.frame_count = RtlCaptureStackBackTrace(
        1,
        static_cast<ULONG>(frames.size()),
        frames.data(),
        nullptr);
    for (std::size_t index = 0; index < event.frame_count; ++index) {
        event.frames[index] = reinterpret_cast<std::uintptr_t>(frames[index]);
    }
}

[[nodiscard]] bool IsTrackedMediaHandle(const HANDLE handle) noexcept {
    for (const auto& tracked : g_media_handles) {
        if (tracked.load(std::memory_order_acquire) == handle) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool IsTrackedNonMediaHandle(const HANDLE handle) noexcept {
    for (const auto& tracked : g_non_media_handles) {
        if (tracked.load(std::memory_order_acquire) == handle) {
            return true;
        }
    }
    return false;
}

void TrackMediaHandle(const HANDLE handle) noexcept {
    if (IsTrackedMediaHandle(handle)) {
        return;
    }
    for (auto& tracked : g_media_handles) {
        HANDLE expected = nullptr;
        if (tracked.compare_exchange_strong(
                expected, handle, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

void TrackNonMediaHandle(const HANDLE handle) noexcept {
    if (IsTrackedNonMediaHandle(handle)) {
        return;
    }
    for (auto& tracked : g_non_media_handles) {
        HANDLE expected = nullptr;
        if (tracked.compare_exchange_strong(
                expected, handle, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

void ForgetTrackedHandle(const HANDLE handle) noexcept {
    const auto forget = [handle](auto& handles) noexcept {
        for (auto& tracked : handles) {
            HANDLE expected = handle;
            static_cast<void>(tracked.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire));
        }
    };
    forget(g_media_handles);
    forget(g_non_media_handles);
}

[[nodiscard]] bool SameFileIdentity(
    const FILE_ID_INFO& left,
    const FILE_ID_INFO& right) noexcept {
    return left.VolumeSerialNumber == right.VolumeSerialNumber &&
        std::memcmp(
            left.FileId.Identifier,
            right.FileId.Identifier,
            sizeof(left.FileId.Identifier)) == 0;
}

[[nodiscard]] bool IdentifyAndTrackMediaHandle(const HANDLE handle) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (IsTrackedMediaHandle(handle)) {
        return true;
    }
    if (IsTrackedNonMediaHandle(handle)) {
        return false;
    }

    FILE_ID_INFO identity{};
    if (GetFileInformationByHandleEx(
        handle,
        FileIdInfo,
        &identity,
        sizeof(identity)) == FALSE) {
        TrackNonMediaHandle(handle);
        return false;
    }
    if (SameFileIdentity(identity, g_target_file_id)) {
        TrackMediaHandle(handle);
        return true;
    }
    TrackNonMediaHandle(handle);
    return false;
}

[[nodiscard]] bool TryGetReadOffset(
    const HANDLE handle,
    const OVERLAPPED* const overlapped,
    std::uint64_t& offset) noexcept {
    if (overlapped != nullptr) {
        OVERLAPPED snapshot{};
        SIZE_T copied = 0;
        if (ReadProcessMemory(
                GetCurrentProcess(),
                overlapped,
                &snapshot,
                sizeof(snapshot),
                &copied) == FALSE || copied != sizeof(snapshot)) {
            return false;
        }
        offset = (static_cast<std::uint64_t>(snapshot.OffsetHigh) << 32) |
            snapshot.Offset;
        return true;
    }

    LARGE_INTEGER distance{};
    LARGE_INTEGER position{};
    if (SetFilePointerEx(handle, distance, &position, FILE_CURRENT) == FALSE ||
        position.QuadPart < 0) {
        return false;
    }
    offset = static_cast<std::uint64_t>(position.QuadPart);
    return true;
}

[[nodiscard]] bool IntersectsTargetSegment(
    const std::uint64_t offset,
    const std::uint32_t byte_count) noexcept {
    if (byte_count == 0) {
        return false;
    }
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t end = byte_count > maximum - offset
        ? maximum
        : offset + byte_count;
    return offset < kMediaSegmentEnd && end > kMediaSegmentStart;
}

#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
[[nodiscard]] std::uintptr_t GameRva(const void* const address) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (g_game_image_base == 0 || value < g_game_image_base) {
        return 0;
    }
    const std::uintptr_t rva = value - g_game_image_base;
    return rva < 0x04000000ull ? rva : 0;
}

[[nodiscard]] bool CopyProcessMemory(
    const void* const source,
    void* const destination,
    const std::size_t size) noexcept {
    if (source == nullptr || destination == nullptr || size == 0) {
        return false;
    }
    SIZE_T copied = 0;
    return ReadProcessMemory(
               GetCurrentProcess(), source, destination, size, &copied) != FALSE &&
        copied == size;
}

void CaptureResourceIdentityScope(
    ResourceIdentityScopeInput& scope) noexcept {
    constexpr std::size_t kObjectFieldsOffset = 0x24;
    constexpr std::size_t kObjectFieldsSize = 12;
    const auto owner = scope.owner;
    constexpr auto maximum = (std::numeric_limits<std::uintptr_t>::max)();
    if (owner != 0 && owner <= maximum - kObjectFieldsOffset) {
        std::array<std::byte, kObjectFieldsSize> fields{};
        if (CopyProcessMemory(
                reinterpret_cast<const void*>(owner + kObjectFieldsOffset),
                fields.data(),
                fields.size())) {
            std::memcpy(
                &scope.member_table_offset,
                fields.data(),
                sizeof(scope.member_table_offset));
            std::memcpy(
                &scope.package_base,
                fields.data() + sizeof(scope.member_table_offset),
                sizeof(scope.package_base));
            scope.object_fields_valid = true;
        }
    }

    void* stream = nullptr;
    if (CopyProcessMemory(
            reinterpret_cast<const void*>(scope.argument5),
            &stream,
            sizeof(stream))) {
        scope.stream = reinterpret_cast<std::uintptr_t>(stream);
        scope.stream_valid = true;
    }
}

class ResourceIdentityScopeGuard final {
public:
    ResourceIdentityScopeGuard(
        ResourceIdentityThreadContext& context,
        const ResourceIdentityScopeInput& input,
        const std::uint64_t sequence) noexcept
        : context_(context) {
        const auto begin = context_.Begin(input, sequence);
        token_ = begin.token;
        if (begin.status == ResourceIdentityBeginStatus::tracked) {
            g_resource_identity_scopes.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_resource_identity_overflows.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ResourceIdentityScopeGuard(const ResourceIdentityScopeGuard&) = delete;
    ResourceIdentityScopeGuard& operator=(const ResourceIdentityScopeGuard&) = delete;

    ~ResourceIdentityScopeGuard() {
        if (context_.End(token_) == ResourceIdentityEndStatus::context_reset) {
            g_resource_identity_context_resets.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    ResourceIdentityThreadContext& context_;
    ResourceIdentityScopeToken token_{};
};

__declspec(noinline) std::int32_t HookResourceIdentity(
    void* const owner,
    void* const argument2,
    void* const argument3,
    void* const argument4,
    void* const argument5) {
    const auto original = g_original_resource_identity.load(std::memory_order_acquire);
    if (original == nullptr) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return 0;
    }

    const DWORD entry_error = GetLastError();
    ResourceIdentityScopeInput scope;
    scope.owner = reinterpret_cast<std::uintptr_t>(owner);
    scope.argument2 = reinterpret_cast<std::uintptr_t>(argument2);
    scope.argument3 = reinterpret_cast<std::uintptr_t>(argument3);
    scope.argument4 = reinterpret_cast<std::uintptr_t>(argument4);
    scope.argument5 = reinterpret_cast<std::uintptr_t>(argument5);
    scope.caller_rva = GameRva(_ReturnAddress());
    CaptureResourceIdentityScope(scope);
    const std::uint64_t sequence =
        g_resource_identity_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    ResourceIdentityScopeGuard guard(
        g_resource_identity_context, scope, sequence);
    SetLastError(entry_error);

    const std::int32_t result = original(
        owner, argument2, argument3, argument4, argument5);
    const DWORD result_error = GetLastError();
    SetLastError(result_error);
    return result;
}

[[nodiscard]] bool ObserveResourceIdentityRead(
    const std::uintptr_t caller_rva,
    void* const stream,
    void* const destination,
    const std::int32_t requested,
    const std::int32_t returned,
    ResourceIdentitySample& sample) noexcept {
    if (caller_rva != kExpectedResourceReadReturnRva) {
        return false;
    }

    ResourceIdentityReadInput read;
    read.stream = reinterpret_cast<std::uintptr_t>(stream);
    read.destination = reinterpret_cast<std::uintptr_t>(destination);
    read.caller_rva = caller_rva;
    read.requested = requested;
    read.returned = returned;
    const auto status = g_resource_identity_context.ObserveRead(read, sample);
    if (status != ResourceIdentityObserveStatus::recorded) {
        return false;
    }
    g_resource_identity_samples.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#if defined(DS2_GENERAL_DDS_ENABLED)
[[nodiscard]] bool TryHashProcessMemory(
    const void* const source,
    const std::size_t size,
    Sha256Digest& digest) noexcept {
    if (source == nullptr || size == 0 ||
        size > kMaxGeneralRuntimeHashBytes) {
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    if (address > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        return false;
    }

    PortableSha256Context context;
    std::array<std::byte, kGeneralRuntimeHashChunkSize> chunk{};
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t count = (std::min)(chunk.size(), size - offset);
        SIZE_T copied = 0;
        if (ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(address + offset),
                chunk.data(), count, &copied) == FALSE ||
            copied != count ||
            !PortableSha256Update(
                context, std::span(chunk.data(), count))) {
            return false;
        }
        offset += count;
    }
    return PortableSha256Finalize(context, digest);
}

#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
struct GeneralDdsWriteResult final {
    bool written{};
    bool verified{};
    std::uint32_t size{};
    DWORD win32_error{};
};

[[nodiscard]] GeneralDdsWriteResult TryApplyGeneralDdsReplacement(
    const GeneralDdsDryRunEvaluation& evaluation,
    void* const destination,
    const std::int32_t requested) noexcept {
    GeneralDdsWriteResult result;
    const std::span<const std::byte> replacement =
        GeneralDdsReplacementBytes(evaluation);
    if (destination == nullptr || requested <= 0 || replacement.empty() ||
        replacement.size() != static_cast<std::size_t>(requested) ||
        replacement.size() >
            static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        result.win32_error = ERROR_INVALID_DATA;
        return result;
    }

    result.size = static_cast<std::uint32_t>(replacement.size());
    SIZE_T written = 0;
    if (WriteProcessMemory(
            GetCurrentProcess(), destination, replacement.data(),
            replacement.size(), &written) == FALSE) {
        result.win32_error = GetLastError();
        return result;
    }
    if (written != replacement.size()) {
        result.win32_error = ERROR_WRITE_FAULT;
        return result;
    }
    result.written = true;

    Sha256Digest observed{};
    if (!TryHashProcessMemory(destination, replacement.size(), observed)) {
        result.win32_error = ERROR_READ_FAULT;
        return result;
    }
    const Sha256Digest& expected =
        evaluation.decision == GeneralDdsDryRunDecision::full_dds
            ? evaluation.candidate->replacement_full_sha256
            : evaluation.candidate->replacement_payload_sha256;
    if (observed != expected) {
        result.win32_error = ERROR_CRC;
        return result;
    }
    result.verified = true;
    return result;
}
#endif

void ObserveGeneralDdsDryRun(
    const ResourceIdentitySample& sample,
    void* const destination,
    const std::int32_t requested,
    const std::int32_t returned,
    const DWORD result_error) noexcept {
    if (!IsResourceIdentitySampleUsable(
            sample,
            kExpectedResourceIdentityOuterReturnRva,
            kExpectedResourceReadReturnRva)) {
        return;
    }

    const PackageResourceIdentity identity{
        sample.scope.package_base,
        static_cast<std::uint32_t>(sample.scope.member_table_offset),
        sample.read_ordinal,
    };
    const GeneralDdsCandidateSnapshot* const candidates =
        g_general_candidates.load(std::memory_order_acquire);
    const GeneralDdsCandidate* const candidate = candidates == nullptr
        ? nullptr
        : candidates->Find(identity);
    if (candidate == nullptr) {
        // Unmapped reads are deliberately silent: the hook observes every
        // member read and emitting them would quickly saturate the POD queue.
        return;
    }

    bool destination_valid = false;
    if (destination != nullptr && requested > 0) {
        destination_valid = IsWritableMemoryRange(
            destination, static_cast<std::size_t>(requested));
    }

    GeneralDdsDryRunInput input{};
    input.identity = identity;
    input.destination_valid = destination_valid;
    input.requested = requested;
    input.returned = returned;
    const bool recognized_size = requested > 0 &&
        (requested == static_cast<std::int32_t>(candidate->original_size) ||
         requested == static_cast<std::int32_t>(candidate->payload_size));
    if (destination_valid && recognized_size && returned == requested) {
        input.source_hash_valid = TryHashProcessMemory(
            destination, static_cast<std::size_t>(requested),
            input.source_sha256);
        if (!input.source_hash_valid) {
            g_general_source_hash_failures.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    const GeneralDdsDryRunEvaluation evaluation = EvaluateGeneralDdsDryRun(
        candidates, input);
    if (evaluation.candidate == nullptr ||
        evaluation.decision == GeneralDdsDryRunDecision::unmapped) {
        return;
    }

    g_general_candidate_hits.fetch_add(1, std::memory_order_relaxed);
    const bool would_override =
        evaluation.decision == GeneralDdsDryRunDecision::full_dds ||
        evaluation.decision == GeneralDdsDryRunDecision::payload;
    if (would_override) {
        g_general_would_override.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_general_contract_mismatches.fetch_add(1, std::memory_order_relaxed);
        if (evaluation.decision ==
            GeneralDdsDryRunDecision::source_hash_mismatch) {
            g_general_source_hash_mismatches.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    GeneralDdsWriteResult write_result;
    bool write_attempted = false;
    // No blocking inside the detour. Concurrent candidates pass through; the
    // winner rehashes under this gate before touching memory. A write fault
    // latches this process into observation until restart.
    if (would_override && g_writer_enabled.load(std::memory_order_acquire) &&
        !g_writer_faulted.load(std::memory_order_acquire) &&
        !g_writer_busy.test_and_set(std::memory_order_acquire)) {
        struct ReleaseGate { ~ReleaseGate() { g_writer_busy.clear(std::memory_order_release); } } gate;
        Sha256Digest current_hash{};
        const bool still_original = TryHashProcessMemory(destination,
            static_cast<std::size_t>(requested), current_hash) && current_hash == input.source_sha256;
        if (!g_writer_faulted.load(std::memory_order_acquire) && still_original) {
        write_attempted = true;
        g_general_write_attempts.fetch_add(1, std::memory_order_relaxed);
        write_result = TryApplyGeneralDdsReplacement(
            evaluation, destination, requested);
        if (write_result.written) {
            g_general_writes_completed.fetch_add(1, std::memory_order_relaxed);
            if (!write_result.verified) {
                g_general_write_verify_failures.fetch_add(
                    1, std::memory_order_relaxed);
            }
        } else {
            g_general_write_failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (!write_result.verified) g_writer_faulted.store(true, std::memory_order_release);
        }
    }
#endif

    EventSlot* const slot = ClaimEventSlot();
    if (slot == nullptr) {
        return;
    }
    ResolverProbeEvent event{};
    event.kind = ResolverProbeEventKind::general_dds_dry_run;
    event.win32_error = result_error;
    event.stream = sample.read.stream;
    event.destination = reinterpret_cast<std::uintptr_t>(destination);
    event.requested = requested;
    event.returned = returned;
    event.call_succeeded = returned == requested;
    event.resource_identity = sample;
    event.general_dds.decision = evaluation.decision;
    event.general_dds.identity = identity;
    event.general_dds.original_size = evaluation.candidate->original_size;
    event.general_dds.payload_size = evaluation.candidate->payload_size;
    event.general_dds.destination_valid = destination_valid;
    event.general_dds.source_hash_valid = input.source_hash_valid;
    event.general_dds.source_sha256 = input.source_sha256;
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    event.general_dds.replacement_attempted = write_attempted;
    event.general_dds.replacement_written = write_result.written;
    event.general_dds.replacement_verified = write_result.verified;
    event.general_dds.replacement_size = write_result.size;
    event.general_dds.replacement_win32_error = write_result.win32_error;
#endif
    CaptureEventContext(event);
    slot->event = event;
    PublishEventSlot(*slot, ResolverProbeEventKind::general_dds_dry_run);
}
#endif
#endif

BOOL WINAPI HookReadFile(
    const HANDLE file,
    const LPVOID buffer,
    const DWORD requested,
    const LPDWORD bytes_read,
    const LPOVERLAPPED overlapped) noexcept {
    g_read_file_calls.fetch_add(1, std::memory_order_relaxed);
    const auto original = g_original_read_file.load(std::memory_order_acquire);
    if (original == nullptr) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }

    const DWORD entry_error = GetLastError();
    const bool is_media = IdentifyAndTrackMediaHandle(file);
    std::uint64_t file_offset = 0;
    const bool has_offset = is_media && TryGetReadOffset(file, overlapped, file_offset);
    SetLastError(entry_error);

    const BOOL result = original(file, buffer, requested, bytes_read, overlapped);
    const DWORD result_error = GetLastError();

    if (has_offset && IntersectsTargetSegment(file_offset, requested)) {
        g_package_segment_reads.fetch_add(1, std::memory_order_relaxed);
        if (EventSlot* const slot = ClaimEventSlot(); slot != nullptr) {
            ResolverProbeEvent event{};
            event.kind = ResolverProbeEventKind::package_segment_read;
            event.file_offset = file_offset;
            event.requested = requested > static_cast<DWORD>((std::numeric_limits<std::int32_t>::max)())
                ? (std::numeric_limits<std::int32_t>::max)()
                : static_cast<std::int32_t>(requested);
            event.destination = reinterpret_cast<std::uintptr_t>(buffer);
            event.call_succeeded = result != FALSE;
            event.win32_error = result_error;
            DWORD transferred = 0;
            SIZE_T copied = 0;
            if (bytes_read != nullptr &&
                ReadProcessMemory(
                    GetCurrentProcess(),
                    bytes_read,
                    &transferred,
                    sizeof(transferred),
                    &copied) != FALSE && copied == sizeof(transferred)) {
                event.returned = transferred > static_cast<DWORD>((std::numeric_limits<std::int32_t>::max)())
                    ? (std::numeric_limits<std::int32_t>::max)()
                    : static_cast<std::int32_t>(transferred);
            }
            CaptureEventContext(event);
            slot->event = event;
            PublishEventSlot(*slot, ResolverProbeEventKind::package_segment_read);
        }
    }

    SetLastError(result_error);
    return result;
}

BOOL WINAPI HookCloseHandle(const HANDLE object) noexcept {
    const auto original = g_original_close_handle.load(std::memory_order_acquire);
    if (original == nullptr) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    // Clear before the kernel can recycle the numeric HANDLE value. If close
    // fails, the next read simply revalidates the still-live handle by file ID.
    ForgetTrackedHandle(object);
    const BOOL result = original(object);
    const DWORD result_error = GetLastError();
    SetLastError(result_error);
    return result;
}

std::int32_t HookStreamRead(
    void* const stream,
    void* const destination,
    const std::int32_t byte_count) {
    const auto original = g_original_stream_read.load(std::memory_order_acquire);
    if (original == nullptr) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return 0;
    }

#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    const std::uintptr_t caller_rva = GameRva(_ReturnAddress());
#endif
    if (!g_reported_stream_first_entry.load(std::memory_order_acquire)) {
        const DWORD entry_error = GetLastError();
        NotifyStreamFirstEntryOnce();
        SetLastError(entry_error);
    }
    const std::int32_t result = original(stream, destination, byte_count);
#if defined(DS2_GENERAL_DDS_ENABLED)
    const DWORD result_error = GetLastError();
#endif
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    ResourceIdentitySample identity_sample;
    const bool identity_correlated = ObserveResourceIdentityRead(
        caller_rva,
        stream,
        destination,
        byte_count,
        result,
        identity_sample);
#if defined(DS2_GENERAL_DDS_ENABLED)
    if (identity_correlated) {
        ObserveGeneralDdsDryRun(
            identity_sample,
            destination,
            byte_count,
            result,
            result_error);
    }
#endif
#endif
    SetLastError(result_error);
    return result;
}

void RecordMinHookStatus(const MH_STATUS status) noexcept {
    g_last_minhook_status.store(static_cast<int>(status), std::memory_order_release);
}

[[nodiscard]] bool RollBackQueuedHooks() noexcept {
    g_writer_enabled.store(false, std::memory_order_release);
    MH_STATUS first_failure = MH_OK;
    const auto queue_disable = [&first_failure](void* const target) noexcept {
        if (target == nullptr) {
            return;
        }
        const MH_STATUS status = MH_QueueDisableHook(target);
        if (status != MH_OK && status != MH_ERROR_DISABLED && first_failure == MH_OK) {
            first_failure = status;
        }
    };
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    queue_disable(g_resource_identity_target);
#endif
    queue_disable(g_read_file_target);
    queue_disable(g_close_handle_target);
    queue_disable(g_stream_read_target);
    const MH_STATUS apply_status = MH_ApplyQueued();
    if (apply_status != MH_OK && first_failure == MH_OK) {
        first_failure = apply_status;
    }
    if (first_failure != MH_OK) {
        RecordMinHookStatus(first_failure);
        return false;
    }
    g_read_file_installed.store(false, std::memory_order_release);
    g_close_handle_installed.store(false, std::memory_order_release);
    g_stream_read_installed.store(false, std::memory_order_release);
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    g_resource_identity_installed.store(false, std::memory_order_release);
#endif
    return true;
}

[[nodiscard]] ResolverProbeState SignatureFailureState(
    const SignatureScanStatus status) noexcept {
    switch (status) {
    case SignatureScanStatus::not_found:
        return ResolverProbeState::signature_not_found;
    case SignatureScanStatus::ambiguous:
        return ResolverProbeState::signature_ambiguous;
    default:
        return ResolverProbeState::signature_mismatch;
    }
}

}  // namespace

void SetResolverProbeEventCallback(
    const ResolverProbeEventCallback callback) noexcept {
    g_event_callback.store(callback, std::memory_order_release);
}

bool InitializeResolverProbe(
    const HMODULE game_module,
    const std::filesystem::path& media_upak_path,
    std::shared_ptr<const GeneralDdsCandidateSnapshot> general_candidates,
    const bool write_enabled) noexcept {
    std::scoped_lock lock(g_lifecycle_mutex);
    const ResolverProbeState current = g_state.load(std::memory_order_acquire);
    if (current == ResolverProbeState::active) {
        return true;
    }
    // Initialization is intentionally one-shot. After any MinHook failure,
    // trampolines may have been created (or briefly entered after ApplyQueued),
    // so retrying creation in the same process would be unsafe and ambiguous.
    if (current != ResolverProbeState::not_initialized) {
        return false;
    }
    if (game_module == nullptr || media_upak_path.empty()) {
        g_state.store(ResolverProbeState::signature_mismatch, std::memory_order_release);
        return false;
    }
    if (!general_candidates || general_candidates->Entries().empty()) {
        g_state.store(ResolverProbeState::invalid_replacement, std::memory_order_release);
        return false;
    }
    g_general_candidates_owner = std::move(general_candidates);
    g_general_candidates.store(g_general_candidates_owner.get(), std::memory_order_release);
    // Publish ownership before activation, but permission only after ALL hooks succeed.
    g_writer_enabled.store(false, std::memory_order_release);

    const HANDLE target_file = CreateFileW(
        media_upak_path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (target_file == INVALID_HANDLE_VALUE) {
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }
    const BOOL identity_result = GetFileInformationByHandleEx(
        target_file,
        FileIdInfo,
        &g_target_file_id,
        sizeof(g_target_file_id));
    const DWORD identity_error = GetLastError();
    CloseHandle(target_file);
    if (identity_result == FALSE) {
        SetLastError(identity_error);
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }

    SignatureParseResult pattern;
    try {
        pattern = ParseSignature(
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 "
            "41 56 41 57 48 83 EC 20 8B 71 14 45 8B F0 48 8B FA 48 8B D9");
    } catch (...) {
        g_state.store(ResolverProbeState::signature_mismatch, std::memory_order_release);
        return false;
    }
    if (!pattern) {
        g_state.store(ResolverProbeState::signature_mismatch, std::memory_order_release);
        return false;
    }
    const SignatureScanResult scan = ScanExecutableSectionsUnique(
        game_module, pattern.pattern, ".text");
    if (!scan) {
        g_state.store(SignatureFailureState(scan.status), std::memory_order_release);
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(game_module);
    const auto target = reinterpret_cast<std::uintptr_t>(scan.address);
    if (target < base || target - base != kExpectedStreamReadRva) {
        g_state.store(ResolverProbeState::signature_mismatch, std::memory_order_release);
        return false;
    }

#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    SignatureParseResult resource_identity_pattern;
    try {
        resource_identity_pattern = ParseSignature(
            "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 55 53 56 57 "
            "41 54 41 55 41 56 41 57 48 8D 68 A8 48 81 EC 18 01 00 00");
    } catch (...) {
        g_state.store(ResolverProbeState::signature_mismatch, std::memory_order_release);
        return false;
    }
    if (!resource_identity_pattern) {
        g_state.store(ResolverProbeState::signature_mismatch, std::memory_order_release);
        return false;
    }
    const SignatureScanResult resource_identity_scan =
        ScanExecutableSectionsUnique(
            game_module, resource_identity_pattern.pattern, ".text");
    if (!resource_identity_scan) {
        g_state.store(
            SignatureFailureState(resource_identity_scan.status),
            std::memory_order_release);
        return false;
    }
    const auto resource_identity_target =
        reinterpret_cast<std::uintptr_t>(resource_identity_scan.address);
    if (resource_identity_target < base ||
        resource_identity_target - base != kExpectedResourceIdentityRva) {
        g_state.store(ResolverProbeState::signature_mismatch, std::memory_order_release);
        return false;
    }

    // The identity contract also depends on two exact call sites. In the
    // supported executable the outer site invokes the OBPK method and the
    // inner site always constructs RCX from the same stack-local wrapper.
    // Verify both in memory before any hook can publish identity evidence.
    const auto matches_expected_callsite = [game_module, base](
        const char* const signature,
        const std::uintptr_t expected_rva) noexcept {
        try {
            const SignatureParseResult parsed = ParseSignature(signature);
            if (!parsed) {
                return false;
            }
            const SignatureScanResult found = ScanExecutableSectionsUnique(
                game_module, parsed.pattern, ".text");
            if (!found) {
                return false;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(
                found.address);
            return address >= base && address - base == expected_rva;
        } catch (...) {
            return false;
        }
    };
    if (!matches_expected_callsite(
            "4C 8D 54 24 58 48 8D 53 10 49 8B 48 30 4D 8D 48 10 "
            "4C 89 54 24 20 48 8B 01 FF 50 20 03 F8",
            kExpectedResourceIdentityOuterCallsiteRva) ||
        !matches_expected_callsite(
            "41 0F BA F0 1F 48 8B 14 33 48 8D 4C 24 70 "
            "E8 EE CC 6A FF 48 83 C3 10",
            kExpectedResourceReadCallsiteRva)) {
        g_state.store(
            ResolverProbeState::signature_mismatch,
            std::memory_order_release);
        return false;
    }
#endif

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }
    const FARPROC read_file_export = GetProcAddress(kernel32, "ReadFile");
    const FARPROC close_handle_export = GetProcAddress(kernel32, "CloseHandle");
    if (read_file_export == nullptr || close_handle_export == nullptr) {
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }
    g_read_file_target = reinterpret_cast<void*>(read_file_export);
    g_close_handle_target = reinterpret_cast<void*>(close_handle_export);
    g_stream_read_target = const_cast<std::byte*>(scan.address);
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    g_resource_identity_target =
        const_cast<std::byte*>(resource_identity_scan.address);
    g_game_image_base = base;
#endif

    const MH_STATUS initialize_status = MH_Initialize();
    RecordMinHookStatus(initialize_status);
    if (initialize_status != MH_OK &&
        initialize_status != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }

    void* original_read_file = nullptr;
    MH_STATUS status = MH_CreateHook(
        g_read_file_target,
        reinterpret_cast<void*>(&HookReadFile),
        &original_read_file);
    RecordMinHookStatus(status);
    if (status != MH_OK) {
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }
    g_original_read_file.store(
        reinterpret_cast<ReadFileFn>(original_read_file),
        std::memory_order_release);

    void* original_close_handle = nullptr;
    status = MH_CreateHook(
        g_close_handle_target,
        reinterpret_cast<void*>(&HookCloseHandle),
        &original_close_handle);
    RecordMinHookStatus(status);
    if (status != MH_OK) {
        static_cast<void>(MH_RemoveHook(g_read_file_target));
        g_original_read_file.store(nullptr, std::memory_order_release);
        g_read_file_target = nullptr;
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }
    g_original_close_handle.store(
        reinterpret_cast<CloseHandleFn>(original_close_handle),
        std::memory_order_release);

    void* original_stream_read = nullptr;
    status = MH_CreateHook(
        g_stream_read_target,
        reinterpret_cast<void*>(&HookStreamRead),
        &original_stream_read);
    RecordMinHookStatus(status);
    if (status != MH_OK) {
        static_cast<void>(MH_RemoveHook(g_close_handle_target));
        static_cast<void>(MH_RemoveHook(g_read_file_target));
        g_original_close_handle.store(nullptr, std::memory_order_release);
        g_original_read_file.store(nullptr, std::memory_order_release);
        g_close_handle_target = nullptr;
        g_read_file_target = nullptr;
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }
    g_original_stream_read.store(
        reinterpret_cast<StreamReadFn>(original_stream_read),
        std::memory_order_release);

#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    void* original_resource_identity = nullptr;
    status = MH_CreateHook(
        g_resource_identity_target,
        reinterpret_cast<void*>(&HookResourceIdentity),
        &original_resource_identity);
    RecordMinHookStatus(status);
    if (status != MH_OK) {
        static_cast<void>(MH_RemoveHook(g_stream_read_target));
        static_cast<void>(MH_RemoveHook(g_close_handle_target));
        static_cast<void>(MH_RemoveHook(g_read_file_target));
        g_original_stream_read.store(nullptr, std::memory_order_release);
        g_original_close_handle.store(nullptr, std::memory_order_release);
        g_original_read_file.store(nullptr, std::memory_order_release);
        g_stream_read_target = nullptr;
        g_close_handle_target = nullptr;
        g_read_file_target = nullptr;
        g_resource_identity_target = nullptr;
        g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        return false;
    }
    g_original_resource_identity.store(
        reinterpret_cast<ResourceIdentityFn>(original_resource_identity),
        std::memory_order_release);
#endif

    status = MH_QueueEnableHook(g_read_file_target);
    RecordMinHookStatus(status);
    if (status == MH_OK) {
        status = MH_QueueEnableHook(g_close_handle_target);
        RecordMinHookStatus(status);
    }
    if (status == MH_OK) {
        status = MH_QueueEnableHook(g_stream_read_target);
        RecordMinHookStatus(status);
    }
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    if (status == MH_OK) {
        status = MH_QueueEnableHook(g_resource_identity_target);
        RecordMinHookStatus(status);
    }
#endif
    if (status != MH_OK) {
        if (!RollBackQueuedHooks()) {
            g_state.store(ResolverProbeState::indeterminate, std::memory_order_release);
        } else {
            g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        }
        return false;
    }

    status = MH_ApplyQueued();
    RecordMinHookStatus(status);
    if (status != MH_OK) {
        if (!RollBackQueuedHooks()) {
            g_state.store(ResolverProbeState::indeterminate, std::memory_order_release);
        } else {
            g_state.store(ResolverProbeState::installation_failed, std::memory_order_release);
        }
        return false;
    }

    g_read_file_installed.store(true, std::memory_order_release);
    g_close_handle_installed.store(true, std::memory_order_release);
    g_stream_read_installed.store(true, std::memory_order_release);
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    g_resource_identity_installed.store(true, std::memory_order_release);
#endif
    g_state.store(ResolverProbeState::active, std::memory_order_release);
    g_writer_enabled.store(write_enabled, std::memory_order_release);
    return true;
}

bool TryPopResolverProbeEvent(ResolverProbeEvent& event) noexcept {
    const std::size_t start = g_next_read_slot.fetch_add(
        1, std::memory_order_relaxed) % kEventSlotCount;
    for (std::size_t offset = 0; offset < kEventSlotCount; ++offset) {
        EventSlot& slot = g_event_slots[(start + offset) % kEventSlotCount];
        SlotState expected = SlotState::ready;
        if (!slot.state.compare_exchange_strong(
                expected,
                SlotState::reading,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        event = slot.event;
        slot.state.store(SlotState::free, std::memory_order_release);
        return true;
    }
    return false;
}

ResolverProbeStatus GetResolverProbeStatus() noexcept {
    ResolverProbeStatus status;
    status.state = g_state.load(std::memory_order_acquire);
    status.read_file_hook_installed =
        g_read_file_installed.load(std::memory_order_acquire);
    status.close_handle_hook_installed =
        g_close_handle_installed.load(std::memory_order_acquire);
    status.stream_read_hook_installed =
        g_stream_read_installed.load(std::memory_order_acquire);
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    status.resource_identity_hook_installed =
        g_resource_identity_installed.load(std::memory_order_acquire);
#endif
    status.last_minhook_status =
        g_last_minhook_status.load(std::memory_order_acquire);
    status.stream_read_rva = kExpectedStreamReadRva;
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    status.resource_identity_rva = kExpectedResourceIdentityRva;
#endif
    return status;
}

ResolverProbeStats GetResolverProbeStats() noexcept {
    ResolverProbeStats stats;
    stats.read_file_calls = g_read_file_calls.load(std::memory_order_relaxed);
    stats.package_segment_reads =
        g_package_segment_reads.load(std::memory_order_relaxed);
    stats.target_sized_stream_reads =
        g_target_sized_stream_reads.load(std::memory_order_relaxed);
    stats.queued_events = g_queued_events.load(std::memory_order_relaxed);
    stats.dropped_events = g_dropped_events.load(std::memory_order_relaxed);
    stats.sample_copy_failures =
        g_sample_copy_failures.load(std::memory_order_relaxed);
    stats.target_hash_matches =
        g_target_hash_matches.load(std::memory_order_relaxed);
    stats.replacements_applied =
        g_replacements_applied.load(std::memory_order_relaxed);
    stats.replacement_write_failures =
        g_replacement_write_failures.load(std::memory_order_relaxed);
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    stats.resource_identity_scopes =
        g_resource_identity_scopes.load(std::memory_order_relaxed);
    stats.resource_identity_samples =
        g_resource_identity_samples.load(std::memory_order_relaxed);
    stats.resource_identity_overflows =
        g_resource_identity_overflows.load(std::memory_order_relaxed);
    stats.resource_identity_context_resets =
        g_resource_identity_context_resets.load(std::memory_order_relaxed);
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
    stats.general_candidate_hits =
        g_general_candidate_hits.load(std::memory_order_relaxed);
    stats.general_would_override =
        g_general_would_override.load(std::memory_order_relaxed);
    stats.general_contract_mismatches =
        g_general_contract_mismatches.load(std::memory_order_relaxed);
    stats.general_source_hash_failures =
        g_general_source_hash_failures.load(std::memory_order_relaxed);
    stats.general_source_hash_mismatches =
        g_general_source_hash_mismatches.load(std::memory_order_relaxed);
    stats.general_write_attempts =
        g_general_write_attempts.load(std::memory_order_relaxed);
    stats.general_writes_completed =
        g_general_writes_completed.load(std::memory_order_relaxed);
    stats.general_write_failures =
        g_general_write_failures.load(std::memory_order_relaxed);
    stats.general_write_verify_failures =
        g_general_write_verify_failures.load(std::memory_order_relaxed);
#endif
    return stats;
}

}  // namespace ds2::modding
