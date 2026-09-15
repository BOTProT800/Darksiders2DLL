#pragma once

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

// Identity observed at the OBPK member-processing boundary. The current
// catalog is deliberately scoped to media.upak, so the package itself is an
// invariant rather than another key field.
struct PackageResourceIdentity final {
    std::uint64_t package_base{};
    std::uint32_t member_table_offset{};
    std::uint32_t member_ordinal{};

    [[nodiscard]] bool operator==(
        const PackageResourceIdentity&) const noexcept = default;
};

struct PackageIdentityEntry final {
    PackageResourceIdentity identity;
    CanonicalVirtualPath virtual_path;
    std::uint64_t segment_size{};
    std::uint64_t uncompressed_offset{};
    std::uint32_t original_size{};
    std::uint32_t type_id{};
};

enum class PackageIdentityCatalogError {
    none,
    invalid_argument,
    manifest_load_failed,
    invalid_manifest,
    package_open_failed,
    invalid_package,
    resource_limit_exceeded,
    allocation_failed,
};

enum class PackageIdentityCatalogIssueCode {
    path_not_in_manifest,
    member_not_found,
    ambiguous_member,
    unsupported_layout,
};

struct PackageIdentityCatalogIssue final {
    PackageIdentityCatalogIssueCode code{
        PackageIdentityCatalogIssueCode::member_not_found};
    std::wstring virtual_path;
    std::wstring detail;
};

struct PackageIdentityCatalogOptions final {
    std::uint64_t max_manifest_bytes{16ull * 1024ull * 1024ull};
    std::size_t max_requested_paths{16'384};
    std::size_t max_manifest_paths{2'000'000};
    std::size_t max_manifest_assets{2'000'000};
    std::size_t max_segments{100'000};
    std::size_t max_members_per_segment{2'000'000};
    std::size_t max_segment_metadata_bytes{64ull * 1024ull * 1024ull};
    std::size_t max_string_bytes{32'767};
    std::size_t max_issue_count{1'024};
};

struct PackageIdentityCatalogBuildResult;

class PackageIdentityCatalogSnapshot final {
public:
    PackageIdentityCatalogSnapshot(const PackageIdentityCatalogSnapshot&) = delete;
    PackageIdentityCatalogSnapshot& operator=(
        const PackageIdentityCatalogSnapshot&) = delete;

    // Both lookups are allocation-free. Returned pointers remain valid for
    // the lifetime of the snapshot.
    [[nodiscard]] const PackageIdentityEntry* Find(
        const PackageResourceIdentity& identity) const noexcept;
    [[nodiscard]] const PackageIdentityEntry* Find(
        const CanonicalVirtualPath& virtual_path) const noexcept;
    [[nodiscard]] const PackageIdentityEntry* Find(
        std::wstring_view canonical_key) const noexcept;

    [[nodiscard]] std::span<const PackageIdentityEntry> Entries() const noexcept {
        return entries_;
    }

private:
    friend struct PackageIdentityCatalogBuildResult;
    friend PackageIdentityCatalogBuildResult BuildMediaPackageIdentityCatalog(
        const std::filesystem::path&,
        std::span<const CanonicalVirtualPath>,
        const PackageIdentityCatalogOptions&);

    PackageIdentityCatalogSnapshot(
        std::vector<PackageIdentityEntry> entries,
        std::vector<std::size_t> path_order) noexcept;

    // Sorted by PackageResourceIdentity.
    std::vector<PackageIdentityEntry> entries_;
    // Indices into entries_, sorted by virtual_path.key.
    std::vector<std::size_t> path_order_;
};

struct PackageIdentityCatalogBuildResult final {
    std::shared_ptr<const PackageIdentityCatalogSnapshot> snapshot;
    PackageIdentityCatalogError error{PackageIdentityCatalogError::none};
    unsigned long system_error{};
    std::wstring detail;
    std::vector<PackageIdentityCatalogIssue> issues;
    std::size_t dropped_issue_count{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot != nullptr && error == PackageIdentityCatalogError::none;
    }
};

// Builds only the mappings requested by the loose-mod snapshot. It parses the
// installed manifest and the metadata prefix of each relevant named OBPK
// segment, never decompresses package payloads and never writes game files.
// Unknown paths and unsupported layouts are reported and omitted; malformed
// trusted package structures fail the whole catalog closed.
[[nodiscard]] PackageIdentityCatalogBuildResult BuildMediaPackageIdentityCatalog(
    const std::filesystem::path& game_directory,
    std::span<const CanonicalVirtualPath> requested_paths,
    const PackageIdentityCatalogOptions& options = {});

[[nodiscard]] std::wstring_view PackageIdentityCatalogErrorName(
    PackageIdentityCatalogError error) noexcept;
[[nodiscard]] std::wstring_view PackageIdentityCatalogIssueCodeName(
    PackageIdentityCatalogIssueCode code) noexcept;

}  // namespace ds2::modding
