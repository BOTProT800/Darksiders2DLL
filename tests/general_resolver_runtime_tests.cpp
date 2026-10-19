// Exercise the production detour implementation without installing hooks or
// opening the game. Win32 fault injection exists only in this test executable.
#include "pch.h"
#include "general_resolver_runtime_tests.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
enum class MemoryFault { none, write_error, partial, corrupt, verify_read, source_read };
MemoryFault memory_fault{};
std::size_t write_calls{};
bool after_write{};

BOOL WINAPI TestReadMemory(HANDLE process, LPCVOID source, LPVOID destination,
                          SIZE_T size, SIZE_T* copied) {
    if (memory_fault == MemoryFault::source_read ||
        (memory_fault == MemoryFault::verify_read && after_write)) {
        *copied = 0;
        SetLastError(ERROR_PARTIAL_COPY);
        return FALSE;
    }
    return ReadProcessMemory(process, source, destination, size, copied);
}

BOOL WINAPI TestWriteMemory(HANDLE process, LPVOID destination, LPCVOID source,
                           SIZE_T size, SIZE_T* written) {
    ++write_calls;
    after_write = true;
    if (memory_fault == MemoryFault::write_error) {
        *written = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const SIZE_T count = memory_fault == MemoryFault::partial ? size - 1 : size;
    const BOOL result = WriteProcessMemory(process, destination, source, count, written);
    if (result && memory_fault == MemoryFault::corrupt) {
        static_cast<std::byte*>(destination)[0] ^= std::byte{0xFF};
    }
    return result;
}

void Check(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}  // namespace

#define ReadProcessMemory TestReadMemory
#define WriteProcessMemory TestWriteMemory
#include "../resolver_probe.cpp"
#undef WriteProcessMemory
#undef ReadProcessMemory

extern "C" std::int32_t InvokeResourceReadFrame(
    std::int32_t (*function)(void*, void*, std::int32_t),
    void* destination, std::int32_t count, std::uint64_t member_index);
// skip_return == 0: the direct read; otherwise the skip path inside 0x100120.
extern "C" std::int32_t InvokeEngineFrame(
    std::int32_t (*function)(void*, void*, std::int32_t),
    void* destination, std::int32_t count, std::uint64_t member_index, std::uintptr_t skip_return);

namespace {
using namespace ds2::modding;

void ResetRuntime() {
    g_writer_enabled = true;
    g_writer_faulted = false;
    g_writer_busy.clear();
    memory_fault = MemoryFault::none;
    after_write = false;
    write_calls = 0;
    g_general_candidate_hits = 0;
    g_general_would_override = 0;
    g_general_contract_mismatches = 0;
    g_general_source_hash_failures = 0;
    g_general_source_hash_mismatches = 0;
    g_general_write_attempts = 0;
    g_general_writes_completed = 0;
    g_general_write_failures = 0;
    g_general_write_verify_failures = 0;
    g_resource_identity_invalid_members = 0;
    g_model_reads = 0; g_model_matches = 0; g_model_write_attempts = 0;
    g_model_writes = 0; g_model_failures = 0;
    g_animation_reads = 0; g_animation_matches = 0;
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    g_model_resize_metadata_patches = 0; g_model_resize_metadata_failures = 0;
    g_model_resize_members = 0; g_model_resize_failures = 0;
    g_texture_resize_metadata_patches = 0; g_texture_resize_metadata_failures = 0;
    g_texture_resize_metadata_declined = 0; g_texture_resize_members = 0; g_texture_resize_failures = 0;
    t_resize_splice = {};
#endif
    ResolverProbeEvent event;
    while (TryPopResolverProbeEvent(event)) {}
}

ResourceIdentitySample MakeSample(const GeneralDdsCandidate& candidate) {
    ResourceIdentitySample sample;
    sample.sequence = 1;
    sample.nesting_depth = 1;
    sample.read_ordinal = 1;
    sample.read.member_ordinal = candidate.identity.member_ordinal;
    sample.scope.package_base = candidate.identity.package_base;
    sample.scope.member_table_offset =
        static_cast<std::int32_t>(candidate.identity.member_table_offset);
    sample.scope.object_fields_valid = true;
    sample.scope.stream_valid = true;
    sample.scope.stream = 1;
    sample.scope.caller_rva = kExpectedResourceIdentityOuterReturnRva;
    sample.read.stream = 2;
    sample.read.caller_rva = kExpectedResourceReadReturnRva;
    return sample;
}

std::int32_t FakeStreamRead(void*, void*, const std::int32_t count) {
    SetLastError(ERROR_IO_PENDING);
    return count;
}

std::uintptr_t recorded_caller{};
std::int32_t RecordCaller(void*, void*, const std::int32_t count) {
    recorded_caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    return count;
}

// Both calls return to the same instruction. This lets the test supply the
// supported caller RVA without installing a hook or fabricating game code.
// Only this negative-test caller disables optimization to prevent a tail call.
// The actual detour above and the valid MASM-frame tests use the build's flags.
#pragma optimize("", off)
__declspec(noinline) std::int32_t InvokeAtSameSite(
    const StreamReadFn function, void* destination, const std::int32_t count) {
    const auto result = function(reinterpret_cast<void*>(2), destination, count);
    return result;
}
#pragma optimize("", on)
}  // namespace

void TestGeneralResolverRuntime(
    const ds2::modding::GeneralDdsCandidateSnapshot& snapshot,
    const ds2::modding::GeneralDdsCandidate& candidate,
    const std::span<const std::byte> original) {
    using namespace ds2::modding;
    g_general_candidates = &snapshot;
    const auto sample = MakeSample(candidate);
    (void)InvokeResourceReadFrame(RecordCaller, nullptr, 0, 0);
    g_game_image_base = recorded_caller - kExpectedResourceReadReturnRva;
    g_original_stream_read = FakeStreamRead;

    for (const bool payload : {false, true}) {
        const auto source = payload ? original.subspan(candidate.payload_offset) : original;
        const auto replacement = payload
            ? candidate.storage->Bytes().subspan(candidate.payload_offset)
            : candidate.storage->Bytes();
        ResetRuntime();
        std::vector<std::byte> destination(source.begin(), source.end());
        const auto count = static_cast<std::int32_t>(destination.size());
        const auto scope = g_resource_identity_context.Begin(sample.scope, 1);
        const auto returned = InvokeResourceReadFrame(
            HookStreamRead, destination.data(), count, candidate.identity.member_ordinal - 1);
        const DWORD stream_error = GetLastError();
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched &&
              returned == count && stream_error == ERROR_IO_PENDING,
              "general stream result/LastError was not preserved");
        ResolverProbeEvent event;
        Check(TryPopResolverProbeEvent(event), "general runtime event missing");
        const auto stats = GetResolverProbeStats();
        Check(stats.general_would_override == 1 &&
              stats.general_contract_mismatches == 0 &&
              event.win32_error == ERROR_IO_PENDING, "general runtime gate/event failed");
        Check(event.resource_identity.read_ordinal == 1 &&
              event.general_dds.identity == candidate.identity &&
              stats.resource_identity_invalid_members == 0,
              "file identity was replaced with the read count");
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
        Check(write_calls == 1 && stats.general_write_attempts == 1 &&
              stats.general_writes_completed == 1 && stats.general_write_failures == 0 &&
              stats.general_write_verify_failures == 0 &&
              event.general_dds.replacement_written && event.general_dds.replacement_verified &&
              event.general_dds.replacement_size == destination.size() &&
              event.general_dds.replacement_win32_error == ERROR_SUCCESS &&
              std::equal(destination.begin(), destination.end(), replacement.begin()),
              "general runtime replacement failed");
#else
        (void)replacement;
        Check(write_calls == 0 && stats.general_write_attempts == 0 &&
              stats.general_writes_completed == 0 && stats.general_write_failures == 0 &&
              stats.general_write_verify_failures == 0 &&
              !event.general_dds.replacement_written && !event.general_dds.replacement_verified &&
              event.general_dds.replacement_size == 0 &&
              std::equal(destination.begin(), destination.end(), source.begin()),
              "observation mode wrote bytes or write counters");
#endif
    }

    // Reproduce the regression: extra inner reads must not advance the file
    // index. Then jump across skipped files. Only the explicitly selected file
    // may match, even when another file has the exact target size and bytes.
    ResetRuntime();
    const auto regression_scope = g_resource_identity_context.Begin(sample.scope, 2);
    std::vector<std::byte> untouched(original.begin(), original.end());
    const auto regression_count = static_cast<std::int32_t>(untouched.size());
    for (std::uint32_t index = 0; index < candidate.identity.member_ordinal + 3; ++index) {
        (void)InvokeResourceReadFrame(HookStreamRead, untouched.data(), regression_count,
                                     kResourceIdentityMaxMembersPerScope - 1);
    }
    Check(g_general_candidate_hits == 0 && write_calls == 0 &&
          std::equal(untouched.begin(), untouched.end(), original.begin()),
          "another file was identified using its read count");
    // Same member has an unrelated small read before its DDS payload.
    std::array<std::byte, 16> small{};
    (void)InvokeResourceReadFrame(HookStreamRead, small.data(), 16,
                                 candidate.identity.member_ordinal - 1);
    (void)InvokeResourceReadFrame(HookStreamRead, untouched.data(), regression_count,
                                 candidate.identity.member_ordinal - 1);
    Check(g_resource_identity_context.End(regression_scope.token) ==
              ResourceIdentityEndStatus::matched &&
          g_general_candidate_hits == 2 && g_general_would_override == 1 &&
          g_general_contract_mismatches == 1,
          "skipped files or multiple reads broke explicit member identity");

    // Invalid indices and caller-frame layouts fail closed in the actual hook.
    for (const std::uint64_t index : {std::uint64_t{kResourceIdentityMaxMembersPerScope},
                                     ~std::uint64_t{0}}) {
        ResetRuntime();
        const auto rejected_scope = g_resource_identity_context.Begin(sample.scope, 3);
        (void)InvokeResourceReadFrame(HookStreamRead, untouched.data(), regression_count, index);
        Check(g_resource_identity_context.End(rejected_scope.token) ==
                  ResourceIdentityEndStatus::matched &&
              g_resource_identity_invalid_members == 1 && write_calls == 0 &&
              g_general_candidate_hits == 0, "invalid member index passed the hook");
    }
    ResetRuntime();
    (void)InvokeAtSameSite(RecordCaller, nullptr, 0);
    g_game_image_base = recorded_caller - kExpectedResourceReadReturnRva;
    const auto bad_frame_scope = g_resource_identity_context.Begin(sample.scope, 4);
    (void)InvokeAtSameSite(HookStreamRead, untouched.data(), regression_count);
    Check(g_resource_identity_context.End(bad_frame_scope.token) ==
              ResourceIdentityEndStatus::matched &&
          g_resource_identity_invalid_members == 1 && write_calls == 0,
          "wrong caller frame passed the hook");
    (void)InvokeResourceReadFrame(RecordCaller, nullptr, 0, 0);
    g_game_image_base = recorded_caller - kExpectedResourceReadReturnRva;

    // An inaccessible frame must not be dereferenced directly or fall back to
    // the old counter. Overflow, null frames and wrong call sites also reject.
    Check(CaptureResourceMemberOrdinal(kExpectedResourceReadReturnRva, 1, 0x79) == 0 &&
          CaptureResourceMemberOrdinal(kExpectedResourceReadReturnRva, 0, 0x78) == 0 &&
          CaptureResourceMemberOrdinal(kExpectedResourceReadReturnRva,
              (std::numeric_limits<std::uintptr_t>::max)(), 0x77) == 0 &&
          CaptureResourceMemberOrdinal(0, 1, 0x79) == 0,
          "invalid member frame did not fail closed");

    // All gates execute in the real observer; failures must leave bytes intact.
    for (int rejection = 0; rejection < 9; ++rejection) {
        ResetRuntime();
        std::vector<std::byte> destination(original.begin(), original.end());
        auto rejected_sample = sample;
        auto count = static_cast<std::int32_t>(destination.size());
        auto returned = count;
        void* target = destination.data();
        if (rejection == 0) rejected_sample.scope.object_fields_valid = false;
        if (rejection == 1) ++rejected_sample.scope.package_base;
        if (rejection == 2) --returned;
        if (rejection == 3) { --count; returned = count; }
        if (rejection == 4) destination.back() ^= std::byte{0xFF};
        if (rejection == 5) target = nullptr;
        if (rejection == 6) memory_fault = MemoryFault::source_read;
        if (rejection == 7) target = reinterpret_cast<void*>(1);
        if (rejection == 8) rejected_sample.read.member_ordinal = 0;
        const auto before = destination;
        ObserveGeneralDdsDryRun(rejected_sample, target, count, returned, ERROR_IO_PENDING);
        Check(write_calls == 0 && GetResolverProbeStats().general_write_attempts == 0 &&
              destination == before, "rejected general runtime read wrote memory");
        if (rejection == 4) {
            Check(GetResolverProbeStats().general_source_hash_mismatches == 1,
                  "runtime source hash mismatch not counted");
        }
        if (rejection == 6) {
            Check(GetResolverProbeStats().general_source_hash_failures == 1,
                  "runtime source hash read failure not counted");
        }
    }

#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    for (int gate = 0; gate < 3; ++gate) {
        ResetRuntime();
        if (gate == 0) g_writer_enabled = false; // observe or incomplete activation
        if (gate == 1) g_writer_faulted = true;
        if (gate == 2) g_writer_busy.test_and_set();
        std::vector<std::byte> destination(original.begin(), original.end());
        const auto count = static_cast<std::int32_t>(destination.size());
        ObserveGeneralDdsDryRun(sample, destination.data(), count, count, ERROR_IO_PENDING);
        Check(write_calls == 0 && GetResolverProbeStats().general_write_attempts == 0 &&
              std::equal(destination.begin(), destination.end(), original.begin()),
              "disabled, faulted or concurrent writer touched memory");
    }
    for (const auto fault : {MemoryFault::write_error, MemoryFault::partial,
                             MemoryFault::corrupt, MemoryFault::verify_read}) {
        ResetRuntime();
        memory_fault = fault;
        std::vector<std::byte> destination(original.begin(), original.end());
        const auto count = static_cast<std::int32_t>(destination.size());
        ObserveGeneralDdsDryRun(sample, destination.data(), count, count, ERROR_IO_PENDING);
        ResolverProbeEvent event;
        Check(TryPopResolverProbeEvent(event), "failed write event missing");
        const bool completed = fault == MemoryFault::corrupt || fault == MemoryFault::verify_read;
        const DWORD expected_error = fault == MemoryFault::write_error ? ERROR_ACCESS_DENIED
            : fault == MemoryFault::partial ? ERROR_WRITE_FAULT
            : fault == MemoryFault::corrupt ? ERROR_CRC : ERROR_READ_FAULT;
        const auto stats = GetResolverProbeStats();
        Check(write_calls == 1 && stats.general_write_attempts == 1 &&
              stats.general_writes_completed == (completed ? 1u : 0u) &&
              stats.general_write_failures == (completed ? 0u : 1u) &&
              stats.general_write_verify_failures == (completed ? 1u : 0u) &&
              event.general_dds.replacement_written == completed &&
              !event.general_dds.replacement_verified &&
              event.general_dds.replacement_win32_error == expected_error,
              "partial/failed/corrupt general write misreported");
        Check(g_writer_faulted.load(), "write failure did not latch writer off");
        memory_fault = MemoryFault::none;
        destination.assign(original.begin(), original.end());
        ObserveGeneralDdsDryRun(sample, destination.data(), count, count, ERROR_IO_PENDING);
        Check(write_calls == 1, "writer resumed after a fault without restart");
    }
#endif

    ResetRuntime();
    g_original_stream_read = FakeStreamRead;
    std::array<std::byte, kTargetDdsSize> buffer{};
    for (const auto count : {16, static_cast<int>(kTargetBc3PayloadSize),
                             static_cast<int>(kTargetDdsSize)}) {
        SetLastError(ERROR_ACCESS_DENIED);
        Check(HookStreamRead(nullptr, buffer.data(), count) == count &&
              GetLastError() == ERROR_IO_PENDING, "stream result/LastError was not preserved");
    }
    g_original_stream_read = nullptr;
    g_game_image_base = 0;
    g_general_candidates = nullptr;
    ResetRuntime();
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    std::cout << "general_runtime_write: PASS (explicit member frame, skipped/multiple reads, full/payload, 9 gates, 4 faults, LastError)\n";
#else
    std::cout << "general_runtime_observation: PASS (explicit member frame, skipped/multiple reads, full/payload unchanged, zero writes, 9 gates, LastError)\n";
#endif
}

void TestModelRuntime(const ds2::modding::ModelCandidate& candidate,
                      const std::span<const std::byte> original) {
    using namespace ds2::modding;
    ModelCandidateSnapshot snapshot;
    snapshot.entries.push_back(candidate);
    g_model_candidates = &snapshot;
    g_original_stream_read = FakeStreamRead;
    (void)InvokeResourceReadFrame(RecordCaller, nullptr, 0, 0);
    g_game_image_base = recorded_caller - kExpectedResourceReadReturnRva;
    ResourceIdentityScopeInput input;
    input.stream_valid = true; input.stream = 1; input.object_fields_valid = true;
    input.package_base = candidate.identity.package_base;
    input.member_table_offset = static_cast<std::int32_t>(candidate.identity.member_table_offset);
    input.caller_rva = kExpectedResourceIdentityOuterReturnRva;
    for (const auto& range : candidate.ranges) {
        for (const bool write : {false, true}) {
            ResetRuntime();
            g_model_write_enabled = write;
            const auto src = original.subspan(range.offset, range.size);
            std::vector<std::byte> buffer(src.begin(), src.end());
            const auto scope = g_resource_identity_context.Begin(input, 4);
            const auto count = static_cast<std::int32_t>(range.size);
            const auto got = InvokeResourceReadFrame(HookStreamRead, buffer.data(), count,
                                                     candidate.identity.member_ordinal - 1);
            Check(got == count && GetLastError() == ERROR_IO_PENDING, "model altered stream result/error");
            Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched,
                  "model scope did not close");
            ResolverProbeEvent event;
            Check(TryPopResolverProbeEvent(event) && event.kind == ResolverProbeEventKind::model_read &&
                  event.model_range_matched && event.model_offset == range.offset && g_model_matches == 1,
                  "model identity/range not matched");
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
            const bool did_write = write && range.original_hash != range.replacement_hash;
#else
            const bool did_write = false;
#endif
            const auto expected = did_write ? candidate.storage->Bytes().subspan(range.offset, range.size) : src;
            Check(event.model_write_verified == did_write && event.model_write_attempted == did_write &&
                  write_calls == (did_write ? 1u : 0u) &&
                  std::equal(buffer.begin(), buffer.end(), expected.begin()), "model write/observe failed");
        }
    }
    // Gates and faults need a range the writer would really replace.
    std::size_t changed = candidate.ranges.size();
    while (changed > 0 && candidate.ranges[changed - 1].original_hash ==
                              candidate.ranges[changed - 1].replacement_hash) --changed;
    Check(changed > 0, "model runtime fixture has no changed range");
    const auto& range = candidate.ranges[--changed];
    ResourceIdentitySample sample;
    sample.sequence = 1; sample.nesting_depth = 1; sample.read_ordinal = 474;
    sample.scope = input; sample.read.stream = 2;
    sample.read.caller_rva = kExpectedResourceReadReturnRva;
    sample.read.member_ordinal = candidate.identity.member_ordinal;
    {
        // A verified read of one of the untouched meshes is not an override.
        ResetRuntime(); g_model_write_enabled = true;
        auto& unchanged = snapshot.entries.front().ranges[changed];
        const auto saved_hash = unchanged.replacement_hash;
        unchanged.replacement_hash = unchanged.original_hash;
        const auto src = original.subspan(range.offset, range.size);
        std::vector<std::byte> buffer(src.begin(), src.end());
        const auto count = static_cast<std::int32_t>(range.size);
        ObserveModelRead(sample, buffer.data(), count, count, ERROR_IO_PENDING);
        ResolverProbeEvent event;
        Check(TryPopResolverProbeEvent(event) && event.model_range_matched &&
              !event.model_range_changed && !event.model_write_attempted && write_calls == 0 &&
              std::equal(buffer.begin(), buffer.end(), src.begin()), "unchanged range reported/written as override");
        unchanged.replacement_hash = saved_hash;
    }
    for (int gate = 0; gate < 9; ++gate) {
        ResetRuntime(); g_model_write_enabled = true;
        const auto src = original.subspan(range.offset, range.size);
        std::vector<std::byte> buffer(src.begin(), src.end());
        auto rejected = sample;
        auto count = static_cast<std::int32_t>(range.size), returned = count;
        void* target = buffer.data();
        if (gate == 0) ++rejected.scope.package_base;
        if (gate == 1) ++rejected.read.member_ordinal;
        if (gate == 2) --returned;
        if (gate == 3) { --count; returned = count; }
        if (gate == 4) buffer[0] ^= std::byte{0xff};
        if (gate == 5) target = reinterpret_cast<void*>(1);
        if (gate == 6) g_writer_enabled = false;
        if (gate == 7) g_writer_faulted = true;
        if (gate == 8) g_writer_busy.test_and_set();
        const auto before = buffer;
        ObserveModelRead(rejected, target, count, returned, ERROR_IO_PENDING);
        Check(buffer == before && write_calls == 0, "rejected model read wrote bytes");
    }
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    for (const auto fault : {MemoryFault::write_error, MemoryFault::partial,
                            MemoryFault::corrupt, MemoryFault::verify_read}) {
        ResetRuntime(); g_model_write_enabled = true; memory_fault = fault;
        const auto src = original.subspan(range.offset, range.size);
        std::vector<std::byte> buffer(src.begin(), src.end());
        const auto count = static_cast<std::int32_t>(range.size);
        ObserveModelRead(sample, buffer.data(), count, count, ERROR_IO_PENDING);
        Check(g_model_failures == 1 && g_writer_faulted && g_model_writes == 0,
              "model fault did not disable shared writer");
        memory_fault = MemoryFault::none;
        buffer.assign(src.begin(), src.end());
        ObserveModelRead(sample, buffer.data(), count, count, ERROR_IO_PENDING);
        Check(write_calls == 1, "model writer resumed after fault");
    }
#endif
    g_model_candidates = nullptr; g_model_write_enabled = false;
    g_original_stream_read = nullptr; g_game_image_base = 0;
    ResetRuntime();
    std::cout << "model_runtime: PASS (explicit identity, full/payload/index/vertex blocks, observe, 9 gates, shared fault latch)\n";
}

namespace {
// The decompressed stream under the inflate wrapper: what the engine would
// read from the package for the member and the one after it.
std::vector<std::byte> fake_stream;
std::size_t fake_cursor{};
std::int32_t SequenceStreamRead(void*, void* destination, const std::int32_t count) {
    const auto n = (std::min)(static_cast<std::size_t>(count), fake_stream.size() - fake_cursor);
    if (destination != nullptr) std::memcpy(destination, fake_stream.data() + fake_cursor, n);
    fake_cursor += n;
    return static_cast<std::int32_t>(n);
}
std::vector<ds2::modding::ResolverProbeEvent> DrainResizeEvents() {
    std::vector<ds2::modding::ResolverProbeEvent> events;
    ds2::modding::ResolverProbeEvent event;
    while (TryPopResolverProbeEvent(event))
        if (event.kind == ds2::modding::ResolverProbeEventKind::model_resize) events.push_back(event);
    return events;
}
}

void TestModelResizeRuntime(const ds2::modding::ModelCandidate& candidate,
                            const std::span<const std::byte> original,
                            const std::span<const std::byte> metadata_block,
                            const std::span<const std::byte> next_member) {
    using namespace ds2::modding;
    Check(candidate.resize.has_value(), "resize runtime fixture has no plan");
    ModelCandidateSnapshot snapshot;
    snapshot.entries.push_back(candidate);
    g_model_candidates = &snapshot;
    g_original_stream_read = SequenceStreamRead;
    (void)InvokeEngineFrame(RecordCaller, nullptr, 0, 0, 0);
    const auto direct_return = recorded_caller;
    (void)InvokeEngineFrame(RecordCaller, nullptr, 0, 0, 1);
    const auto skip_return = recorded_caller;
    ResourceIdentityScopeInput input;
    input.stream_valid = true; input.stream = 1; input.object_fields_valid = true;
    input.package_base = candidate.identity.package_base;
    input.member_table_offset = static_cast<std::int32_t>(candidate.identity.member_table_offset);
    input.caller_rva = kExpectedResourceIdentityOuterReturnRva;
    const std::uint64_t index = candidate.identity.member_ordinal - 1;
    const auto replacement = candidate.storage->Bytes();
    const auto& resize = *candidate.resize;
    // One call site for every engine read keeps the frame, and so the
    // wrapper address, identical across direct reads and skips.
    const auto engine = [&](void* destination, const std::uint32_t size, const std::uint64_t member,
                            const bool skip) {
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
        g_game_image_base = skip ? skip_return - kStreamSkipReadReturnRva
                                 : direct_return - kExpectedResourceReadReturnRva;
        const std::uintptr_t skip_call = skip ? g_game_image_base + kStreamSkipCallReturnRva : 0;
#else
        g_game_image_base = direct_return - kExpectedResourceReadReturnRva;
        const std::uintptr_t skip_call = 0;
        static_cast<void>(skip);
#endif
        return InvokeEngineFrame(HookStreamRead, destination, static_cast<std::int32_t>(size), member, skip_call);
    };
    const auto read_metadata = [&](std::vector<std::byte>& block) {
        fake_stream.assign(metadata_block.begin(), metadata_block.end()); fake_cursor = 0;
        block.assign(metadata_block.size(), std::byte{});
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
        g_game_image_base = direct_return - kModelMetadataReadReturnRva;
#endif
        return InvokeEngineFrame(HookStreamRead, block.data(), static_cast<std::int32_t>(block.size()), 0, 0);
    };
    const auto load_stream = [&] {
        fake_stream.assign(original.begin(), original.end());
        fake_stream.insert(fake_stream.end(), next_member.begin(), next_member.end());
        fake_cursor = 0;
    };
    ResetRuntime();
    g_model_write_enabled = true;
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    g_model_resize_patched = std::make_unique<std::atomic<std::uint32_t>[]>(1);
    {
        // Without a metadata patch the engine keeps the original counts.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 7);
        std::vector<std::byte> header(resize.replacement_header_size);
        Check(engine(header.data(), resize.replacement_header_size, index, false) ==
                  static_cast<std::int32_t>(resize.replacement_header_size) &&
                  fake_cursor == resize.replacement_header_size && DrainResizeEvents().empty(),
              "unpatched resize member was spliced");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    // The metadata read: same length, only this member's counts rewritten.
    std::vector<std::byte> block;
    Check(read_metadata(block) == static_cast<std::int32_t>(metadata_block.size()) &&
              g_model_resize_patched[0] == 1 && g_model_resize_metadata_patches == 1,
          "resize metadata not patched");
    auto expected_block = std::vector<std::byte>(metadata_block.begin(), metadata_block.end());
    std::copy(resize.extra.begin(), resize.extra.end(), expected_block.begin() + resize.extra_offset);
    auto events = DrainResizeEvents();
    Check(block == expected_block && events.size() == 1 &&
              events[0].model_resize.phase == ModelResizePhase::metadata_patched, "resize metadata block differs");
    {
        // A header of the original size: the engine used another copy of the
        // counts, so the member is read unchanged.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 8);
        std::vector<std::byte> header(resize.original_header_size);
        Check(engine(header.data(), resize.original_header_size, index, false) ==
                  static_cast<std::int32_t>(resize.original_header_size) &&
                  std::equal(header.begin(), header.end(), original.begin()) && DrainResizeEvents().empty(),
              "original-count header was spliced");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    {
        // The data loop over the patched counts: direct reads and skips.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 9);
        std::size_t offset = 0;
        std::array<std::byte, 8> scratch{};
        for (const auto& buffer : resize.buffers) {
            std::int32_t got = 0;
            if (buffer.skipped) {
                got = engine(scratch.data(), buffer.size, index, true);
            } else {
                std::vector<std::byte> destination(buffer.size);
                got = engine(destination.data(), buffer.size, index, false);
                Check(std::equal(destination.begin(), destination.end(), replacement.begin() + offset),
                      "resized buffer differs from the replacement");
            }
            Check(got == static_cast<std::int32_t>(buffer.size), "resized read count differs");
            offset += buffer.size;
        }
        Check(offset == replacement.size() && fake_cursor == original.size(),
              "resized member did not consume exactly the original");
        std::vector<std::byte> after(next_member.size());
        Check(engine(after.data(), static_cast<std::uint32_t>(after.size()), index + 1, false) ==
                  static_cast<std::int32_t>(after.size()) &&
                  std::equal(after.begin(), after.end(), next_member.begin()),
              "next member lost its alignment");
        events = DrainResizeEvents();
        Check(events.size() == 2 && events[0].model_resize.phase == ModelResizePhase::begin &&
                  events[0].model_resize.original_verified &&
                  events[1].model_resize.phase == ModelResizePhase::complete &&
                  events[1].model_resize.delivered == replacement.size() &&
                  events[1].model_resize.operations == resize.buffers.size() && g_model_resize_members == 1,
              "resize begin/complete events differ");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    {
        // Moving on after the header: the splice ends, the stream stays aligned.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 10);
        std::vector<std::byte> header(resize.replacement_header_size), after(next_member.size());
        Check(engine(header.data(), resize.replacement_header_size, index, false) ==
                  static_cast<std::int32_t>(resize.replacement_header_size), "resized header not served");
        Check(engine(after.data(), static_cast<std::uint32_t>(after.size()), index + 1, false) ==
                  static_cast<std::int32_t>(after.size()) &&
                  std::equal(after.begin(), after.end(), next_member.begin()),
              "next member after an incomplete splice lost its alignment");
        events = DrainResizeEvents();
        Check(events.size() == 2 && events[1].model_resize.phase == ModelResizePhase::incomplete &&
                  g_model_resize_failures == 1, "incomplete splice not reported");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    {
        // A latched writer fault disables both the patch and the splice.
        ResetRuntime(); g_writer_faulted = true;
        g_model_resize_patched = std::make_unique<std::atomic<std::uint32_t>[]>(1);
        Check(read_metadata(block) == static_cast<std::int32_t>(metadata_block.size()) &&
                  std::equal(block.begin(), block.end(), metadata_block.begin()) && g_model_resize_patched[0] == 0,
              "faulted writer patched metadata");
    }
    g_model_resize_patched.reset();
#else
    // Observation builds never patch metadata nor splice members.
    std::vector<std::byte> block(metadata_block.size());
    fake_stream.assign(metadata_block.begin(), metadata_block.end()); fake_cursor = 0;
    g_game_image_base = direct_return - kExpectedResourceReadReturnRva;
    Check(InvokeEngineFrame(HookStreamRead, block.data(), static_cast<std::int32_t>(block.size()), 0, 0) ==
              static_cast<std::int32_t>(block.size()) &&
              std::equal(block.begin(), block.end(), metadata_block.begin()), "observation build changed metadata");
    static_cast<void>(skip_return); static_cast<void>(index); static_cast<void>(engine); static_cast<void>(replacement);
    static_cast<void>(resize); static_cast<void>(read_metadata);
    static_cast<void>(load_stream); static_cast<void>(input);
#endif
    g_model_candidates = nullptr; g_model_write_enabled = false;
    g_original_stream_read = nullptr; g_game_image_base = 0;
    ResetRuntime();
    std::cout << "model_resize_runtime: PASS (metadata patch, original-count passthrough, direct/skip splice, alignment, incomplete, fault latch)\n";
}

namespace {
std::vector<ds2::modding::ResolverProbeEvent> DrainTextureResizeEvents() {
    std::vector<ds2::modding::ResolverProbeEvent> events;
    ds2::modding::ResolverProbeEvent event;
    while (TryPopResolverProbeEvent(event))
        if (event.kind == ds2::modding::ResolverProbeEventKind::texture_resize) events.push_back(event);
    return events;
}
}

void TestTextureResizeRuntime(const ds2::modding::GeneralDdsCandidateSnapshot& snapshot,
                              const std::span<const std::vector<std::byte>> originals,
                              const std::span<const std::byte> metadata_block,
                              const std::span<const std::byte> earlier,
                              const std::span<const std::byte> next_member) {
    using namespace ds2::modding;
    const auto entries = snapshot.Entries();
    Check(entries.size() == 2 && originals.size() == 2 && entries[0].resize && entries[1].resize &&
              entries[0].identity.member_ordinal == 1 && entries[1].identity.member_ordinal == 2,
          "texture resize runtime fixture needs two resized members");
    g_general_candidates = &snapshot;
    g_original_stream_read = SequenceStreamRead;
    (void)InvokeEngineFrame(RecordCaller, nullptr, 0, 0, 0);
    const auto direct_return = recorded_caller;
    (void)InvokeEngineFrame(RecordCaller, nullptr, 0, 0, 1);
    const auto skip_return = recorded_caller;
    ResourceIdentityScopeInput input;
    input.stream_valid = true; input.stream = 1; input.object_fields_valid = true;
    input.package_base = entries[0].identity.package_base;
    input.member_table_offset = static_cast<std::int32_t>(entries[0].identity.member_table_offset);
    input.caller_rva = kExpectedResourceIdentityOuterReturnRva;
    // The handler's mip settings, read through the same pointers as in the game.
    std::uint16_t max_mip_levels = 0;
    std::int32_t texture_detail = 2;
    const auto* texture_detail_address = &texture_detail;
    const auto engine = [&](void* destination, const std::size_t size, const std::uint64_t member, const bool skip) {
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
        g_game_image_base = skip ? skip_return - kStreamSkipReadReturnRva
                                 : direct_return - kExpectedResourceReadReturnRva;
        const std::uintptr_t skip_call = skip ? g_game_image_base + kStreamSkipCallReturnRva : 0;
#else
        g_game_image_base = direct_return - kExpectedResourceReadReturnRva;
        const std::uintptr_t skip_call = 0;
        static_cast<void>(skip);
#endif
        return InvokeEngineFrame(HookStreamRead, destination, static_cast<std::int32_t>(size), member, skip_call);
    };
    // The decompressed stream: bytes still pending from earlier members, both
    // originals, then the next member.
    const auto load_stream = [&] {
        fake_stream.assign(earlier.begin(), earlier.end());
        for (const auto& original : originals) fake_stream.insert(fake_stream.end(), original.begin(), original.end());
        fake_stream.insert(fake_stream.end(), next_member.begin(), next_member.end());
        fake_cursor = 0;
    };
    std::vector<std::byte> scratch(8192);
    // A member's null buffers ({null,128} header) are skipped together with
    // whatever earlier members left pending, right before its first read.
    const auto skip_header = [&](const std::uint64_t member, const std::size_t pending) {
        Check(engine(scratch.data(), pending + 128, member, true) == static_cast<std::int32_t>(pending + 128),
              "texture header skip count differs");
    };
    const auto payload = [](const GeneralDdsCandidate& c) { return c.resize->storage->Bytes().subspan(128); };
    // An original-layout read still takes the copy route: the conversion to
    // the original contract when there is one.
    const auto fallback = [&](const std::size_t member) {
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
        if (entries[member].converted) return entries[member].storage->Bytes().subspan(128);
#endif
        return std::span<const std::byte>(originals[member]).subspan(128);
    };
    ResetRuntime();
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
    g_max_mip_levels_address = reinterpret_cast<std::uintptr_t>(&max_mip_levels);
    g_texture_detail_pointer_address = reinterpret_cast<std::uintptr_t>(&texture_detail_address);
    const auto read_metadata = [&](std::vector<std::byte>& block) {
        fake_stream.assign(metadata_block.begin(), metadata_block.end()); fake_cursor = 0;
        block.assign(metadata_block.size(), std::byte{});
        g_game_image_base = direct_return - kModelMetadataReadReturnRva;
        return InvokeEngineFrame(HookStreamRead, block.data(), static_cast<std::int32_t>(block.size()), 0, 0);
    };
    g_texture_resize_patched = std::make_unique<std::atomic<std::uint32_t>[]>(2);
    {
        // Without a metadata patch the engine reads the original payload.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 7);
        skip_header(0, earlier.size());
        std::vector<std::byte> original_payload(originals[0].size() - 128);
        Check(engine(original_payload.data(), original_payload.size(), 0, false) ==
                  static_cast<std::int32_t>(original_payload.size()) &&
                  std::ranges::equal(original_payload, fallback(0)) &&
                  DrainTextureResizeEvents().empty(), "unpatched texture member was spliced");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    // The metadata read: both members' extras rewritten from one hash of the block.
    std::vector<std::byte> block;
    Check(read_metadata(block) == static_cast<std::int32_t>(metadata_block.size()) &&
              g_texture_resize_patched[0] == 1 && g_texture_resize_patched[1] == 1 &&
              g_texture_resize_metadata_patches == 2, "texture resize metadata not patched");
    auto expected_block = std::vector<std::byte>(metadata_block.begin(), metadata_block.end());
    for (const auto& c : entries)
        std::copy(c.resize->extra.begin(), c.resize->extra.end(), expected_block.begin() + c.resize->extra_offset);
    auto events = DrainTextureResizeEvents();
    Check(block == expected_block && events.size() == 2 &&
              events[0].model_resize.phase == ModelResizePhase::metadata_patched &&
              events[1].model_resize.phase == ModelResizePhase::metadata_patched, "texture metadata block differs");
    {
        // A payload of the original layout: the engine used another copy of
        // the entry, so the member is read unchanged.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 8);
        skip_header(0, earlier.size());
        std::vector<std::byte> original_payload(originals[0].size() - 128);
        Check(engine(original_payload.data(), original_payload.size(), 0, false) ==
                  static_cast<std::int32_t>(original_payload.size()) &&
                  std::ranges::equal(original_payload, fallback(0)) &&
                  DrainTextureResizeEvents().empty(), "original-layout payload was spliced");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    {
        // The data loop over the patched entries: each payload in one run
        // (contiguous texture memory), the next member still aligned.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 9);
        std::size_t pending = earlier.size();
        for (std::uint64_t member = 0; member < 2; ++member) {
            skip_header(member, pending);
            pending = 0;
            const auto expected = payload(entries[member]);
            std::vector<std::byte> destination(expected.size());
            Check(engine(destination.data(), destination.size(), member, false) ==
                      static_cast<std::int32_t>(destination.size()) &&
                      std::equal(destination.begin(), destination.end(), expected.begin()),
                  "resized texture payload differs from the replacement");
        }
        Check(fake_cursor == earlier.size() + originals[0].size() + originals[1].size(),
              "resized textures did not consume exactly the originals");
        std::vector<std::byte> after(next_member.size());
        Check(engine(after.data(), after.size(), 2, false) == static_cast<std::int32_t>(after.size()) &&
                  std::equal(after.begin(), after.end(), next_member.begin()), "next member lost its alignment");
        events = DrainTextureResizeEvents();
        Check(events.size() == 4 && g_texture_resize_members == 2 && g_texture_resize_failures == 0,
              "texture resize events differ");
        for (std::size_t member = 0; member < 2; ++member) {
            const auto& begin = events[member * 2].model_resize;
            const auto& complete = events[member * 2 + 1].model_resize;
            Check(begin.phase == ModelResizePhase::begin && begin.original_verified &&
                      begin.first_request == payload(entries[member]).size() &&
                      complete.phase == ModelResizePhase::complete && complete.operations == 1 &&
                      complete.delivered == entries[member].resize->storage->Size() &&
                      complete.original_size == originals[member].size() &&
                      complete.identity == entries[member].identity, "texture begin/complete events differ");
        }
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    {
        // Separate level buffers: the first read is the top level only.
        load_stream();
        const auto scope = g_resource_identity_context.Begin(input, 10);
        skip_header(0, earlier.size());
        const auto expected = payload(entries[0]);
        const auto top = entries[0].resize->replacement_runs.front();
        Check(top < expected.size(), "texture fixture needs more than one level");
        std::vector<std::byte> first(top), rest(expected.size() - top);
        Check(engine(first.data(), first.size(), 0, false) == static_cast<std::int32_t>(first.size()) &&
                  engine(rest.data(), rest.size(), 0, false) == static_cast<std::int32_t>(rest.size()) &&
                  std::equal(first.begin(), first.end(), expected.begin()) &&
                  std::equal(rest.begin(), rest.end(), expected.begin() + top), "per-level texture reads differ");
        events = DrainTextureResizeEvents();
        Check(events.size() == 2 && events[1].model_resize.phase == ModelResizePhase::complete &&
                  events[1].model_resize.operations == 2, "per-level texture reads not completed");
        // Moving on after the top level: the splice ends, the stream stays aligned.
        skip_header(1, 0);
        std::vector<std::byte> partial(entries[1].resize->replacement_runs.front());
        Check(partial.size() < payload(entries[1]).size() &&
                  engine(partial.data(), partial.size(), 1, false) == static_cast<std::int32_t>(partial.size()) &&
                  std::equal(partial.begin(), partial.end(), payload(entries[1]).begin()),
              "second texture not served");
        std::vector<std::byte> after(next_member.size());
        Check(engine(after.data(), after.size(), 2, false) == static_cast<std::int32_t>(after.size()) &&
                  std::equal(after.begin(), after.end(), next_member.begin()),
              "next member after an incomplete texture lost its alignment");
        events = DrainTextureResizeEvents();
        Check(events.size() == 2 && events[1].model_resize.phase == ModelResizePhase::incomplete &&
                  g_texture_resize_failures == 1, "incomplete texture splice not reported");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    }
    {
        // Mip trimming the handler would apply to a replacement keeps that
        // member's original entry (and clears its earlier patch).
        ResetRuntime();
        const auto& first = entries[0].resize->dds;
        max_mip_levels = static_cast<std::uint16_t>(first.mip_count - 1);
        Check(TextureEngineMipSkip(first.mip_count, first.width, first.height, max_mip_levels, 2) == 1,
              "fixture replacement is not trimmed by MaxMipLevels");
        const bool second_trimmed = TextureEngineMipSkip(entries[1].resize->dds.mip_count,
            entries[1].resize->dds.width, entries[1].resize->dds.height, max_mip_levels, 2) != 0;
        Check(read_metadata(block) == static_cast<std::int32_t>(metadata_block.size()) &&
                  g_texture_resize_patched[0] == 0 && g_texture_resize_metadata_declined >= 1 &&
                  std::equal(block.begin() + entries[0].resize->extra_offset,
                             block.begin() + entries[0].resize->extra_offset + kTextureEngineExtraSize,
                             metadata_block.begin() + entries[0].resize->extra_offset),
              "trimmed texture entry was patched");
        Check(second_trimmed ? g_texture_resize_patched[1] == 0 : g_texture_resize_patched[1] == 2,
              "untrimmed texture entry not patched alongside");
        events = DrainTextureResizeEvents();
        Check(!events.empty() && events[0].model_resize.phase == ModelResizePhase::metadata_declined,
              "texture decline not reported");
        max_mip_levels = 0;
        // Unreadable settings count as trimming.
        g_texture_detail_pointer_address = 0;
        Check(read_metadata(block) == static_cast<std::int32_t>(metadata_block.size()) &&
                  std::equal(block.begin(), block.end(), metadata_block.begin()) && g_texture_resize_patched[0] == 0 &&
                  g_texture_resize_patched[1] == 0, "unreadable mip settings did not keep the original entries");
        g_texture_detail_pointer_address = reinterpret_cast<std::uintptr_t>(&texture_detail_address);
        static_cast<void>(DrainTextureResizeEvents());
    }
    {
        // A latched writer fault disables both the patch and the splice.
        ResetRuntime(); g_writer_faulted = true;
        g_texture_resize_patched = std::make_unique<std::atomic<std::uint32_t>[]>(2);
        Check(read_metadata(block) == static_cast<std::int32_t>(metadata_block.size()) &&
                  std::equal(block.begin(), block.end(), metadata_block.begin()) && g_texture_resize_patched[0] == 0,
              "faulted writer patched texture metadata");
    }
    g_texture_resize_patched.reset();
    g_max_mip_levels_address = 0; g_texture_detail_pointer_address = 0;
#else
    // Observation builds never patch metadata nor splice members.
    load_stream();
    const auto scope = g_resource_identity_context.Begin(input, 7);
    skip_header(0, earlier.size());
    std::vector<std::byte> original_payload(originals[0].size() - 128);
    Check(engine(original_payload.data(), original_payload.size(), 0, false) ==
              static_cast<std::int32_t>(original_payload.size()) &&
              std::ranges::equal(original_payload, fallback(0)) &&
              DrainTextureResizeEvents().empty(), "observation build spliced a texture");
    Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched, "scope lost");
    static_cast<void>(metadata_block); static_cast<void>(payload); static_cast<void>(max_mip_levels);
    static_cast<void>(texture_detail_address); static_cast<void>(skip_return);
#endif
    g_general_candidates = nullptr;
    g_original_stream_read = nullptr; g_game_image_base = 0;
    ResetRuntime();
    std::cout << "texture_resize_runtime: PASS (shared block hash, original-layout passthrough, header skip, "
                 "one-run and per-level splice, alignment, incomplete, mip-trim decline, fault latch)\n";
}

void TestAnimationRuntime(const ds2::modding::AnimationCandidate& candidate,
                          const std::span<const std::byte> original) {
    using namespace ds2::modding;
    AnimationCandidateSnapshot snapshot;
    snapshot.entries.push_back(candidate);
    g_animation_candidates = &snapshot;
    g_original_stream_read = FakeStreamRead;
    (void)InvokeResourceReadFrame(RecordCaller, nullptr, 0, 0);
    g_game_image_base = recorded_caller - kExpectedResourceReadReturnRva;
    ResourceIdentityScopeInput input;
    input.stream_valid = true; input.stream = 1; input.object_fields_valid = true;
    input.package_base = candidate.identity.package_base;
    input.member_table_offset = static_cast<std::int32_t>(candidate.identity.member_table_offset);
    input.caller_rva = kExpectedResourceIdentityOuterReturnRva;
    // Reads through the real detour. Model and shared writers stay armed: the
    // animation branch must observe every range without touching the buffer.
    const auto hooked_read = [&](const std::size_t offset, const std::size_t size) {
        ResetRuntime();
        g_model_write_enabled = true;
        std::vector<std::byte> buffer(original.begin() + static_cast<std::ptrdiff_t>(offset),
                                      original.begin() + static_cast<std::ptrdiff_t>(offset + size));
        const auto before = buffer;
        const auto scope = g_resource_identity_context.Begin(input, 4);
        const auto count = static_cast<std::int32_t>(size);
        const auto got = InvokeResourceReadFrame(HookStreamRead, buffer.data(), count,
                                                 candidate.identity.member_ordinal - 1);
        Check(got == count && GetLastError() == ERROR_IO_PENDING, "animation altered stream result/error");
        Check(g_resource_identity_context.End(scope.token) == ResourceIdentityEndStatus::matched,
              "animation scope did not close");
        ResolverProbeEvent event;
        Check(TryPopResolverProbeEvent(event) && event.kind == ResolverProbeEventKind::animation_read &&
              g_animation_reads == 1, "animation read not observed");
        Check(write_calls == 0 && buffer == before, "animation observation wrote bytes");
        return event;
    };
    for (const auto& range : candidate.ranges) {
        const auto event = hooked_read(range.offset, range.size);
        Check(event.animation_range_matched && event.animation_offset == range.offset &&
              event.animation_range_changed == (range.original_hash != range.replacement_hash) &&
              event.resource_identity.read.hash_valid &&
              event.resource_identity.read.sha256 == range.original_hash && g_animation_matches == 1,
              "animation identity/range not matched");
    }
    // A read the catalog did not predict still reports its size and hash.
    const std::size_t offset = 20, size = 100;
    const auto partial = hooked_read(offset, size);
    const auto expected = ComputeSha256(original.subspan(offset, size));
    Check(!partial.animation_range_matched && g_animation_matches == 0 && expected &&
          partial.resource_identity.read.hash_valid &&
          partial.resource_identity.read.sha256 == expected.digest && partial.requested == 100,
          "unpredicted animation range not hashed");
    ResourceIdentitySample sample;
    sample.sequence = 1; sample.nesting_depth = 1; sample.read_ordinal = 610;
    sample.scope = input; sample.read.stream = 2;
    sample.read.caller_rva = kExpectedResourceReadReturnRva;
    sample.read.member_ordinal = candidate.identity.member_ordinal;
    std::vector<std::byte> buffer(original.begin(), original.end());
    buffer.push_back(std::byte{});
    const auto whole = static_cast<std::int32_t>(original.size());
    for (int gate = 0; gate < 7; ++gate) {
        ResetRuntime(); g_model_write_enabled = true;
        auto rejected = sample;
        auto count = whole, returned = whole;
        void* target = buffer.data();
        if (gate == 0) ++rejected.scope.package_base;
        if (gate == 1) ++rejected.read.member_ordinal;
        if (gate == 2) rejected.read.caller_rva = 0;
        if (gate == 3) --returned;
        if (gate == 4) { ++count; returned = count; }
        if (gate == 5) target = reinterpret_cast<void*>(1);
        if (gate == 6) { count = 0; returned = 0; }
        const auto before = buffer;
        ObserveAnimationRead(rejected, target, count, returned, ERROR_IO_PENDING);
        ResolverProbeEvent event;
        const bool published = TryPopResolverProbeEvent(event);
        Check(buffer == before && write_calls == 0 && g_animation_matches == 0,
              "rejected animation read matched/wrote");
        Check(gate < 3 ? !published && g_animation_reads == 0
                       : published && !event.animation_range_matched && !event.resource_identity.read.hash_valid,
              "rejected animation read misreported");
    }
    g_animation_candidates = nullptr; g_model_write_enabled = false;
    g_original_stream_read = nullptr; g_game_image_base = 0;
    ResetRuntime();
    std::cout << "animation_runtime: PASS (explicit identity, full/header/body, unpredicted range hash, 7 gates, never writes)\n";
}
