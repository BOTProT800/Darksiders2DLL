#include "general_dds_candidate.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace ds2::modding {
namespace {

[[nodiscard]] bool IdentityLess(
    const PackageResourceIdentity& left,
    const PackageResourceIdentity& right) noexcept {
    if (left.package_base != right.package_base) {
        return left.package_base < right.package_base;
    }
    if (left.member_table_offset != right.member_table_offset) {
        return left.member_table_offset < right.member_table_offset;
    }
    return left.member_ordinal < right.member_ordinal;
}

void AddIssue(
    GeneralDdsCandidateBuildResult& result,
    const GeneralDdsCandidateOptions& options,
    const PackageIdentityEntry& package_entry,
    const GeneralDdsCandidateIssueCode code,
    const std::wstring_view mod_id,
    const std::wstring_view detail) {
    if (result.issues.size() >= options.max_issue_count) {
        if (result.dropped_issue_count !=
            (std::numeric_limits<std::size_t>::max)()) {
            ++result.dropped_issue_count;
        }
        return;
    }
    result.issues.push_back(GeneralDdsCandidateIssue{
        code,
        package_entry.identity,
        package_entry.virtual_path.display,
        std::wstring(mod_id),
        std::wstring(detail),
    });
}

[[nodiscard]] bool FitsSignedReadCount(const std::uint64_t size) noexcept {
    return size <= static_cast<std::uint64_t>(
        (std::numeric_limits<std::int32_t>::max)());
}

}  // namespace

GeneralDdsCandidateSnapshot::GeneralDdsCandidateSnapshot(
    std::vector<GeneralDdsCandidate> entries) noexcept
    : entries_(std::move(entries)) {}

const GeneralDdsCandidate* GeneralDdsCandidateSnapshot::Find(
    const PackageResourceIdentity& identity) const noexcept {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), identity,
        [](const GeneralDdsCandidate& entry,
           const PackageResourceIdentity& key) noexcept {
            return IdentityLess(entry.identity, key);
        });
    if (found == entries_.end() || !(found->identity == identity)) {
        return nullptr;
    }
    return std::addressof(*found);
}

GeneralDdsCandidateBuildResult BuildGeneralDdsCandidateSnapshot(
    const PackageIdentityCatalogSnapshot* const package_catalog,
    const PackageDdsContractCatalogSnapshot* const original_contracts,
    const ModIndexSnapshot* const mod_index,
    const GeneralDdsCandidateOptions& options) {
    GeneralDdsCandidateBuildResult result;
    if (package_catalog == nullptr || original_contracts == nullptr ||
        mod_index == nullptr) {
        result.error = GeneralDdsCandidateError::invalid_argument;
        return result;
    }

    try {
        std::vector<GeneralDdsCandidate> entries;
        entries.reserve(package_catalog->Entries().size());
        for (const auto& package_entry : package_catalog->Entries()) {
            if (package_entry.type_id != 6) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::unsupported_type, {},
                    L"package member is not a DDS resource");
                continue;
            }

            const PackageDdsContractEntry* const original =
                original_contracts->Find(package_entry.identity);
            if (original == nullptr) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::missing_original_contract, {},
                    L"installed package DDS contract is unavailable");
                continue;
            }
            const DdsMetadata& original_dds = original->dds;
            if (original->virtual_path.key != package_entry.virtual_path.key ||
                original_dds.expected_file_size != package_entry.original_size ||
                original_dds.header_size !=
                    (original_dds.has_dx10_header ? 148u : 128u) ||
                original_dds.payload_size + original_dds.header_size !=
                    original_dds.expected_file_size) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::original_contract_mismatch, {},
                    L"installed package DDS contract is inconsistent with its member");
                continue;
            }
            if (original_dds.has_dx10_header) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::dx10_header_not_supported, {},
                    L"installed original uses a DX10 DDS header outside the current stream contract");
                continue;
            }

            const IndexedAsset* const asset =
                mod_index->Find(package_entry.virtual_path);
            if (asset == nullptr) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::mod_asset_not_found, {},
                    L"no winning loose mod asset exists for the package path");
                continue;
            }
            if (asset->storage == nullptr) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::invalid_storage,
                    asset->mod_id,
                    L"winning mod asset has no immutable byte storage");
                continue;
            }
            if (!asset->dds.has_value()) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::missing_dds_metadata,
                    asset->mod_id,
                    L"winning mod asset was not validated as DDS");
                continue;
            }

            const auto storage_size = static_cast<std::uint64_t>(
                asset->storage->Size());
            if (storage_size != package_entry.original_size) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::original_size_mismatch,
                    asset->mod_id,
                    L"mod DDS byte count differs from the package member size");
                continue;
            }

            const DdsMetadata& dds = *asset->dds;
            if (dds.has_dx10_header) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::dx10_header_not_supported,
                    asset->mod_id,
                    L"DX10 DDS headers are outside the current stream contract");
                continue;
            }

            constexpr std::uint64_t kTraditionalDdsHeaderSize = 128;
            if (storage_size < kTraditionalDdsHeaderSize ||
                dds.header_size != kTraditionalDdsHeaderSize ||
                dds.expected_file_size != storage_size ||
                dds.payload_size != storage_size - kTraditionalDdsHeaderSize) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::invalid_dds_metadata,
                    asset->mod_id,
                    L"validated DDS metadata is inconsistent with its storage");
                continue;
            }

            if (dds.width != original_dds.width ||
                dds.height != original_dds.height ||
                dds.mip_count != original_dds.mip_count ||
                dds.format != original_dds.format ||
                dds.header_size != original_dds.header_size ||
                dds.payload_size != original_dds.payload_size ||
                dds.expected_file_size != original_dds.expected_file_size ||
                dds.has_dx10_header != original_dds.has_dx10_header) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::original_contract_mismatch,
                    asset->mod_id,
                    L"mod DDS metadata differs from the installed original");
                continue;
            }

            if (!FitsSignedReadCount(storage_size) ||
                !FitsSignedReadCount(dds.header_size) ||
                !FitsSignedReadCount(dds.payload_size)) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::size_not_representable,
                    asset->mod_id,
                    L"DDS sizes do not fit the signed stream-read count");
                continue;
            }

            const auto replacement_payload = asset->storage->Bytes().subspan(
                static_cast<std::size_t>(dds.header_size),
                static_cast<std::size_t>(dds.payload_size));
            const Sha256Result replacement_payload_hash =
                ComputeSha256(replacement_payload);
            if (!replacement_payload_hash) {
                AddIssue(
                    result, options, package_entry,
                    GeneralDdsCandidateIssueCode::replacement_hash_failed,
                    asset->mod_id,
                    L"cannot hash the validated replacement DDS payload");
                continue;
            }

            entries.push_back(GeneralDdsCandidate{
                package_entry.identity,
                package_entry.virtual_path,
                asset->mod_id,
                package_entry.original_size,
                dds,
                asset->storage,
                static_cast<std::uint32_t>(dds.header_size),
                static_cast<std::uint32_t>(dds.payload_size),
                original->full_sha256,
                original->payload_sha256,
                asset->storage->Sha256(),
                replacement_payload_hash.digest,
            });
        }

        std::sort(
            entries.begin(), entries.end(),
            [](const GeneralDdsCandidate& left,
               const GeneralDdsCandidate& right) noexcept {
                return IdentityLess(left.identity, right.identity);
            });
        result.snapshot = std::shared_ptr<const GeneralDdsCandidateSnapshot>(
            new GeneralDdsCandidateSnapshot(std::move(entries)));
        return result;
    } catch (const std::bad_alloc&) {
        result.snapshot.reset();
        result.error = GeneralDdsCandidateError::allocation_failed;
        return result;
    } catch (const std::length_error&) {
        result.snapshot.reset();
        result.error = GeneralDdsCandidateError::allocation_failed;
        return result;
    }
}

std::span<const std::byte> GeneralDdsReplacementBytes(
    const GeneralDdsDryRunEvaluation& evaluation) noexcept {
    const GeneralDdsCandidate* const candidate = evaluation.candidate;
    if (candidate == nullptr || candidate->storage == nullptr) {
        return {};
    }
    const auto bytes = candidate->storage->Bytes();
    if (evaluation.decision == GeneralDdsDryRunDecision::full_dds) {
        return bytes.size() == candidate->original_size ? bytes
                                                        : std::span<const std::byte>{};
    }
    if (evaluation.decision != GeneralDdsDryRunDecision::payload ||
        candidate->payload_offset > bytes.size() ||
        candidate->payload_size > bytes.size() - candidate->payload_offset) {
        return {};
    }
    return bytes.subspan(candidate->payload_offset, candidate->payload_size);
}

GeneralDdsDryRunEvaluation EvaluateGeneralDdsDryRun(
    const GeneralDdsCandidateSnapshot* const snapshot,
    const GeneralDdsDryRunInput& input) noexcept {
    if (snapshot == nullptr) {
        return {GeneralDdsDryRunDecision::invalid, nullptr};
    }
    const GeneralDdsCandidate* const candidate = snapshot->Find(input.identity);
    if (candidate == nullptr) {
        return {GeneralDdsDryRunDecision::unmapped, nullptr};
    }
    if (!input.destination_valid || input.requested <= 0 || input.returned < 0 ||
        input.returned > input.requested) {
        return {GeneralDdsDryRunDecision::invalid, candidate};
    }
    const bool full_dds =
        input.requested == static_cast<std::int32_t>(candidate->original_size);
    const bool payload =
        input.requested == static_cast<std::int32_t>(candidate->payload_size);
    if (!full_dds && !payload) {
        return {GeneralDdsDryRunDecision::size_mismatch, candidate};
    }
    if (input.returned != input.requested) {
        return {GeneralDdsDryRunDecision::short_read, candidate};
    }
    if (!input.source_hash_valid) {
        return {GeneralDdsDryRunDecision::invalid, candidate};
    }
    const Sha256Digest& expected_hash = full_dds
        ? candidate->original_full_sha256
        : candidate->original_payload_sha256;
    if (input.source_sha256 != expected_hash) {
        return {GeneralDdsDryRunDecision::source_hash_mismatch, candidate};
    }
    return {
        full_dds
            ? GeneralDdsDryRunDecision::full_dds
            : GeneralDdsDryRunDecision::payload,
        candidate,
    };
}

std::wstring_view GeneralDdsCandidateErrorName(
    const GeneralDdsCandidateError error) noexcept {
    switch (error) {
    case GeneralDdsCandidateError::none: return L"none";
    case GeneralDdsCandidateError::invalid_argument: return L"invalid_argument";
    case GeneralDdsCandidateError::allocation_failed: return L"allocation_failed";
    }
    return L"unknown";
}

std::wstring_view GeneralDdsCandidateIssueCodeName(
    const GeneralDdsCandidateIssueCode code) noexcept {
    switch (code) {
    case GeneralDdsCandidateIssueCode::unsupported_type:
        return L"unsupported_type";
    case GeneralDdsCandidateIssueCode::missing_original_contract:
        return L"missing_original_contract";
    case GeneralDdsCandidateIssueCode::original_contract_mismatch:
        return L"original_contract_mismatch";
    case GeneralDdsCandidateIssueCode::mod_asset_not_found:
        return L"mod_asset_not_found";
    case GeneralDdsCandidateIssueCode::missing_dds_metadata:
        return L"missing_dds_metadata";
    case GeneralDdsCandidateIssueCode::invalid_storage:
        return L"invalid_storage";
    case GeneralDdsCandidateIssueCode::original_size_mismatch:
        return L"original_size_mismatch";
    case GeneralDdsCandidateIssueCode::dx10_header_not_supported:
        return L"dx10_header_not_supported";
    case GeneralDdsCandidateIssueCode::invalid_dds_metadata:
        return L"invalid_dds_metadata";
    case GeneralDdsCandidateIssueCode::size_not_representable:
        return L"size_not_representable";
    case GeneralDdsCandidateIssueCode::replacement_hash_failed:
        return L"replacement_hash_failed";
    }
    return L"unknown";
}

std::wstring_view GeneralDdsDryRunDecisionName(
    const GeneralDdsDryRunDecision decision) noexcept {
    switch (decision) {
    case GeneralDdsDryRunDecision::invalid: return L"invalid";
    case GeneralDdsDryRunDecision::unmapped: return L"unmapped";
    case GeneralDdsDryRunDecision::full_dds: return L"full_dds";
    case GeneralDdsDryRunDecision::payload: return L"payload";
    case GeneralDdsDryRunDecision::short_read: return L"short_read";
    case GeneralDdsDryRunDecision::size_mismatch: return L"size_mismatch";
    case GeneralDdsDryRunDecision::source_hash_mismatch:
        return L"source_hash_mismatch";
    }
    return L"unknown";
}

}  // namespace ds2::modding
