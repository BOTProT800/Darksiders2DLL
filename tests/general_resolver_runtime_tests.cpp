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
    ResolverProbeEvent event;
    while (TryPopResolverProbeEvent(event)) {}
}

ResourceIdentitySample MakeSample(const GeneralDdsCandidate& candidate) {
    ResourceIdentitySample sample;
    sample.sequence = 1;
    sample.nesting_depth = 1;
    sample.read_ordinal = candidate.identity.member_ordinal;
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
__declspec(noinline) std::int32_t InvokeAtSameSite(
    const StreamReadFn function, void* destination, const std::int32_t count) {
    const auto result = function(reinterpret_cast<void*>(2), destination, count);
    return result;
}
}  // namespace

void TestGeneralResolverRuntime(
    const ds2::modding::GeneralDdsCandidateSnapshot& snapshot,
    const ds2::modding::GeneralDdsCandidate& candidate,
    const std::span<const std::byte> original) {
    using namespace ds2::modding;
    g_general_candidates = &snapshot;
    const auto sample = MakeSample(candidate);
    (void)InvokeAtSameSite(RecordCaller, nullptr, 0);
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
        const auto returned = InvokeAtSameSite(HookStreamRead, destination.data(), count);
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

    // All gates execute in the real observer; failures must leave bytes intact.
    for (int rejection = 0; rejection < 8; ++rejection) {
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
    std::cout << "general_runtime_write: PASS (full/payload, 8 gates, 4 faults, LastError)\n";
#else
    std::cout << "general_runtime_observation: PASS (full/payload unchanged, zero writes, 8 gates, LastError)\n";
#endif
}
