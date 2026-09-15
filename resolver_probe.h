#pragma once

#include "byte_storage.h"
#include "loader_features.h"
#if defined(DS2_GENERAL_DDS_ENABLED) && \
    !defined(DS2_RESOURCE_IDENTITY_ENABLED)
#error DS2_GENERAL_DDS_ENABLED requires DS2_RESOURCE_IDENTITY_ENABLED
#endif
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED) && \
    !defined(DS2_GENERAL_DDS_ENABLED)
#error DS2_GENERAL_DDS_WRITE_ENABLED requires DS2_GENERAL_DDS_ENABLED
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
#include "general_dds_candidate.h"
#endif
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
#include "resource_identity.h"
#endif
#include "resolver_probe_filter.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <type_traits>

namespace ds2::modding {

inline constexpr std::size_t kResolverProbeMaxFrames = 16;

enum class ResolverProbeState {
    not_initialized,
    active,
    signature_not_found,
    signature_ambiguous,
    signature_mismatch,
    invalid_replacement,
    installation_failed,
    indeterminate,
};

enum class ResolverProbeEventKind {
    stream_first_entry,
    package_segment_read,
    stream_sample,
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    resource_identity_sample,
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
    general_dds_dry_run,
#endif
};

#if defined(DS2_GENERAL_DDS_ENABLED)
// Self-contained, trivially-copyable evidence produced by the Debug-only
// general resolver. The default prototype is observation-only. A distinct,
// explicit write prototype may also report the write and its post-write hash.
struct GeneralDdsDryRunProbeEvent final {
    GeneralDdsDryRunDecision decision{GeneralDdsDryRunDecision::invalid};
    PackageResourceIdentity identity{};
    std::uint32_t original_size{};
    std::uint32_t payload_size{};
    bool destination_valid{};
    bool source_hash_valid{};
    Sha256Digest source_sha256{};
    bool replacement_attempted{};
    bool replacement_written{};
    bool replacement_verified{};
    std::uint32_t replacement_size{};
    std::uint32_t replacement_win32_error{};
};

static_assert(std::is_trivially_copyable_v<GeneralDdsDryRunProbeEvent>);
#endif

struct ResolverProbeEvent final {
    ResolverProbeEventKind kind{ResolverProbeEventKind::stream_sample};
    ResolverSampleKind sample_kind{ResolverSampleKind::none};
    std::int64_t performance_counter{};
    std::uint32_t thread_id{};
    std::uint32_t win32_error{};
    std::uintptr_t stream{};
    std::uintptr_t destination{};
    std::uint64_t file_offset{};
    std::int32_t requested{};
    std::int32_t returned{};
    bool call_succeeded{};
    bool target_hash_matched{};
    bool replacement_applied{};
    std::uint16_t frame_count{};
    std::array<std::uintptr_t, kResolverProbeMaxFrames> frames{};
    std::uint32_t sample_size{};
    std::array<std::byte, kTargetDdsSize> sample{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    ResourceIdentitySample resource_identity{};
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
    GeneralDdsDryRunProbeEvent general_dds{};
#endif
};

static_assert(std::is_trivially_copyable_v<ResolverProbeEvent>);

struct ResolverProbeStatus final {
    ResolverProbeState state{ResolverProbeState::not_initialized};
    bool read_file_hook_installed{};
    bool close_handle_hook_installed{};
    bool stream_read_hook_installed{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    bool resource_identity_hook_installed{};
#endif
    int last_minhook_status{};
    std::uintptr_t stream_read_rva{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    std::uintptr_t resource_identity_rva{};
#endif
};

struct ResolverProbeStats final {
    std::uint64_t read_file_calls{};
    std::uint64_t package_segment_reads{};
    std::uint64_t target_sized_stream_reads{};
    std::uint64_t queued_events{};
    std::uint64_t dropped_events{};
    std::uint64_t sample_copy_failures{};
    std::uint64_t target_hash_matches{};
    std::uint64_t replacements_applied{};
    std::uint64_t replacement_write_failures{};
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
    std::uint64_t resource_identity_scopes{};
    std::uint64_t resource_identity_samples{};
    std::uint64_t resource_identity_overflows{};
    std::uint64_t resource_identity_context_resets{};
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
    std::uint64_t general_candidate_hits{};
    std::uint64_t general_would_override{};
    std::uint64_t general_contract_mismatches{};
    std::uint64_t general_source_hash_failures{};
    std::uint64_t general_source_hash_mismatches{};
    std::uint64_t general_write_attempts{};
    std::uint64_t general_writes_completed{};
    std::uint64_t general_write_failures{};
    std::uint64_t general_write_verify_failures{};
#endif
};

// Runs inside a detour. It must only publish POD state and signal an existing
// event. Allocation, symbol lookup and filesystem I/O are forbidden. The
// general prototype may perform bounded, allocation-free SHA-256 after safely
// copying a mapped candidate's source bytes in fixed-size chunks. Only the
// separately opted-in Debug write prototype may replace a verified range.
using ResolverProbeEventCallback = void (*)(ResolverProbeEventKind event) noexcept;

void SetResolverProbeEventCallback(
    ResolverProbeEventCallback callback) noexcept;

// Installs the narrow two-level override for the one allow-listed executable.
// ReadFile records accesses touching the known media.upak segment. The unique
// internal stream reader requires a catalog identity, compatible contract and
// original SHA-256. Writer permission is published after complete activation.
[[nodiscard]] bool InitializeResolverProbe(
    HMODULE game_module,
    const std::filesystem::path& media_upak_path,
    std::shared_ptr<const GeneralDdsCandidateSnapshot> general_candidates,
    bool write_enabled = true) noexcept;

[[nodiscard]] bool TryPopResolverProbeEvent(
    ResolverProbeEvent& event) noexcept;
[[nodiscard]] ResolverProbeStatus GetResolverProbeStatus() noexcept;
[[nodiscard]] ResolverProbeStats GetResolverProbeStats() noexcept;

}  // namespace ds2::modding
