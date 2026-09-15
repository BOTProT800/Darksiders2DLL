#pragma once

#include "byte_storage.h"
#include "dds_validation.h"
#include "mod_index.h"
#include "package_dds_contract.h"
#include "package_identity_catalog.h"
#include "virtual_path.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ds2::modding {

// Immutable join between one package member and the winning loose DDS asset.
// All sizes are checked to fit the game's signed 32-bit stream-read contract
// before an entry can reach this snapshot.
struct GeneralDdsCandidate final {
    PackageResourceIdentity identity;
    CanonicalVirtualPath virtual_path;
    std::wstring mod_id;
    std::uint32_t original_size{};
    DdsMetadata dds;
    std::shared_ptr<const ByteStorage> storage;
    std::uint32_t payload_offset{};
    std::uint32_t payload_size{};
    Sha256Digest original_full_sha256{};
    Sha256Digest original_payload_sha256{};
    Sha256Digest replacement_full_sha256{};
    Sha256Digest replacement_payload_sha256{};
};

enum class GeneralDdsCandidateIssueCode {
    unsupported_type,
    missing_original_contract,
    original_contract_mismatch,
    mod_asset_not_found,
    missing_dds_metadata,
    invalid_storage,
    original_size_mismatch,
    dx10_header_not_supported,
    invalid_dds_metadata,
    size_not_representable,
    replacement_hash_failed,
};

struct GeneralDdsCandidateIssue final {
    GeneralDdsCandidateIssueCode code{
        GeneralDdsCandidateIssueCode::mod_asset_not_found};
    PackageResourceIdentity identity;
    std::wstring virtual_path;
    std::wstring mod_id;
    std::wstring detail;
};

enum class GeneralDdsCandidateError {
    none,
    invalid_argument,
    allocation_failed,
};

struct GeneralDdsCandidateOptions final {
    std::size_t max_issue_count{1'024};
};

struct GeneralDdsCandidateBuildResult;

class GeneralDdsCandidateSnapshot final {
public:
    GeneralDdsCandidateSnapshot(const GeneralDdsCandidateSnapshot&) = delete;
    GeneralDdsCandidateSnapshot& operator=(
        const GeneralDdsCandidateSnapshot&) = delete;

    // Allocation-free. The returned pointer remains valid while the snapshot
    // remains alive.
    [[nodiscard]] const GeneralDdsCandidate* Find(
        const PackageResourceIdentity& identity) const noexcept;

    [[nodiscard]] std::span<const GeneralDdsCandidate> Entries() const noexcept {
        return entries_;
    }

private:
    friend GeneralDdsCandidateBuildResult BuildGeneralDdsCandidateSnapshot(
        const PackageIdentityCatalogSnapshot*,
        const PackageDdsContractCatalogSnapshot*,
        const ModIndexSnapshot*,
        const GeneralDdsCandidateOptions&);

    explicit GeneralDdsCandidateSnapshot(
        std::vector<GeneralDdsCandidate> entries) noexcept;

    // Sorted by PackageResourceIdentity.
    std::vector<GeneralDdsCandidate> entries_;
};

struct GeneralDdsCandidateBuildResult final {
    std::shared_ptr<const GeneralDdsCandidateSnapshot> snapshot;
    GeneralDdsCandidateError error{GeneralDdsCandidateError::none};
    std::wstring detail;
    std::vector<GeneralDdsCandidateIssue> issues;
    std::size_t dropped_issue_count{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot != nullptr && error == GeneralDdsCandidateError::none;
    }
};

// Builds a self-contained immutable snapshot. A package entry that cannot be
// joined safely is omitted and reported at most once; such a rejection is not
// a global build failure. Only bad pointers and allocation failures fail the
// complete build. This join requires an independently recovered contract from
// the installed package and proves equal format, dimensions, mips, surface,
// header policy and payload size. Original DDS/payload hashes are retained for
// the runtime content gate. It still provides dry-run evidence only and never
// authorizes a write by itself.
[[nodiscard]] GeneralDdsCandidateBuildResult BuildGeneralDdsCandidateSnapshot(
    const PackageIdentityCatalogSnapshot* package_catalog,
    const PackageDdsContractCatalogSnapshot* original_contracts,
    const ModIndexSnapshot* mod_index,
    const GeneralDdsCandidateOptions& options = {});

enum class GeneralDdsDryRunDecision {
    invalid,
    unmapped,
    full_dds,
    payload,
    short_read,
    size_mismatch,
    source_hash_mismatch,
};

struct GeneralDdsDryRunInput final {
    PackageResourceIdentity identity;
    // Computed by the caller for the complete requested range. The pure
    // evaluator does not inspect process memory itself.
    bool destination_valid{};
    std::int32_t requested{};
    std::int32_t returned{};
    bool source_hash_valid{};
    Sha256Digest source_sha256{};
};

struct GeneralDdsDryRunEvaluation final {
    GeneralDdsDryRunDecision decision{GeneralDdsDryRunDecision::invalid};
    const GeneralDdsCandidate* candidate{};
};

// Pure, allocation-free classifier. It never reads from or writes to the
// destination. A mapped read is valid only when the caller validated the
// complete destination range, counts are sane, the original read completed,
// its size matches either the complete DDS or its payload, and the observed
// source hash equals the contract recovered from the installed package.
[[nodiscard]] GeneralDdsDryRunEvaluation EvaluateGeneralDdsDryRun(
    const GeneralDdsCandidateSnapshot* snapshot,
    const GeneralDdsDryRunInput& input) noexcept;

// Returns the exact immutable replacement range selected by a successful
// evaluation. Empty means that the decision/candidate/storage contract is not
// internally consistent. It performs no allocation.
[[nodiscard]] std::span<const std::byte> GeneralDdsReplacementBytes(
    const GeneralDdsDryRunEvaluation& evaluation) noexcept;

[[nodiscard]] std::wstring_view GeneralDdsCandidateErrorName(
    GeneralDdsCandidateError error) noexcept;
[[nodiscard]] std::wstring_view GeneralDdsCandidateIssueCodeName(
    GeneralDdsCandidateIssueCode code) noexcept;
[[nodiscard]] std::wstring_view GeneralDdsDryRunDecisionName(
    GeneralDdsDryRunDecision decision) noexcept;

}  // namespace ds2::modding
