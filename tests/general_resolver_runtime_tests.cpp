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
