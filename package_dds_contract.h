#pragma once

#include "dds_validation.h"
#include "package_identity_catalog.h"
#include "sha256.h"
#include "virtual_path.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ds2::modding {

// Immutable contract recovered from the installed media.upak bytes. The
// catalog is read-only and currently accepts only structurally identified
// single-stream OBPK segments. Per-member block layouts remain fail-closed
// until their runtime delivery contract is demonstrated independently.
struct PackageDdsContractEntry final {
    PackageResourceIdentity identity;
    CanonicalVirtualPath virtual_path;
    DdsMetadata dds;
    Sha256Digest full_sha256{};
    Sha256Digest payload_sha256{};
};

enum class PackageDdsContractCatalogError {
    none,
    invalid_argument,
    package_open_failed,
    invalid_package,
    read_failed,
    decompression_failed,
    hash_failed,
    allocation_failed,
};

enum class PackageDdsContractCatalogIssueCode {
    unsupported_type,
    asset_too_large,
    total_budget_exceeded,
    stream_too_large,
    invalid_original_dds,
};

struct PackageDdsContractCatalogIssue final {
    PackageDdsContractCatalogIssueCode code{
        PackageDdsContractCatalogIssueCode::invalid_original_dds};
    PackageResourceIdentity identity;
    std::wstring virtual_path;
    std::wstring detail;
};

struct PackageDdsContractCatalogOptions final {
    std::uint64_t max_asset_bytes{128ull * 1024ull * 1024ull};
    std::uint64_t max_total_asset_bytes{1024ull * 1024ull * 1024ull};
    std::uint64_t max_compressed_stream_bytes{1024ull * 1024ull * 1024ull};
    std::uint64_t max_uncompressed_stream_bytes{1024ull * 1024ull * 1024ull};
    std::size_t max_issue_count{1'024};
};

struct PackageDdsContractCatalogBuildResult;

class PackageDdsContractCatalogSnapshot final {
public:
    PackageDdsContractCatalogSnapshot(
        const PackageDdsContractCatalogSnapshot&) = delete;
    PackageDdsContractCatalogSnapshot& operator=(
        const PackageDdsContractCatalogSnapshot&) = delete;

    [[nodiscard]] const PackageDdsContractEntry* Find(
        const PackageResourceIdentity& identity) const noexcept;

    [[nodiscard]] std::span<const PackageDdsContractEntry> Entries() const noexcept {
        return entries_;
    }

private:
    friend struct PackageDdsContractCatalogBuildResult;
    friend PackageDdsContractCatalogBuildResult BuildMediaPackageDdsContractCatalog(
        const std::filesystem::path&,
        const PackageIdentityCatalogSnapshot*,
        const PackageDdsContractCatalogOptions&);

    explicit PackageDdsContractCatalogSnapshot(
        std::vector<PackageDdsContractEntry> entries) noexcept;

    std::vector<PackageDdsContractEntry> entries_;
};

struct PackageDdsContractCatalogBuildResult final {
    std::shared_ptr<const PackageDdsContractCatalogSnapshot> snapshot;
    PackageDdsContractCatalogError error{
        PackageDdsContractCatalogError::none};
    unsigned long system_error{};
    int zlib_error{};
    std::wstring detail;
    std::vector<PackageDdsContractCatalogIssue> issues;
    std::size_t dropped_issue_count{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot != nullptr &&
            error == PackageDdsContractCatalogError::none;
    }
};

// Reads media.upak without write sharing, inflates each requested single-stream
// segment once, and retains only requested DDS member intervals. Every stream
// must reach a valid zlib EOF and exactly its declared output size. Invalid DDS
// members are omitted individually; malformed package/stream data fails the
// complete build closed.
[[nodiscard]] PackageDdsContractCatalogBuildResult
BuildMediaPackageDdsContractCatalog(
    const std::filesystem::path& game_directory,
    const PackageIdentityCatalogSnapshot* package_catalog,
    const PackageDdsContractCatalogOptions& options = {});

[[nodiscard]] std::wstring_view PackageDdsContractCatalogErrorName(
    PackageDdsContractCatalogError error) noexcept;
[[nodiscard]] std::wstring_view PackageDdsContractCatalogIssueCodeName(
    PackageDdsContractCatalogIssueCode code) noexcept;

}  // namespace ds2::modding
