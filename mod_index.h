#pragma once

#include "byte_storage.h"
#include "dds_validation.h"
#include "virtual_path.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ds2::modding {

struct IndexedAsset final {
    std::wstring mod_id;
    std::wstring mod_key;
    std::size_t mod_priority{};
    CanonicalVirtualPath virtual_path;
    std::filesystem::path source_path;
    std::shared_ptr<const ByteStorage> storage;
    std::optional<DdsMetadata> dds;
};

enum class ModIndexIssueCode {
    unsafe_mod_id,
    duplicate_mod_id,
    reparse_point,
    unsafe_virtual_path,
    filesystem_error,
    not_regular_file,
    byte_load_failed,
    invalid_dds,
    duplicate_asset_in_mod,
    resource_limit_exceeded,
};

struct ModIndexIssue final {
    ModIndexIssueCode code{ModIndexIssueCode::filesystem_error};
    std::wstring mod_id;
    std::filesystem::path path;
    unsigned long system_error{};
    std::wstring detail;
};

enum class ModIndexError {
    none,
    invalid_root,
    root_reparse_point,
    filesystem_error,
    resource_limit_exceeded,
    allocation_failed,
};

struct ModIndexOptions final {
    bool dds_only{false};
    std::uint64_t max_asset_file_size{128ull * 1024ull * 1024ull};
    std::uint64_t max_total_bytes{1024ull * 1024ull * 1024ull};
    std::size_t max_asset_count{16'384};
    std::size_t max_entry_count{100'000};
    std::size_t max_issue_count{1'024};
    std::size_t max_depth{32};
};

class ModIndexSnapshot final {
public:
    ModIndexSnapshot(const ModIndexSnapshot&) = delete;
    ModIndexSnapshot& operator=(const ModIndexSnapshot&) = delete;

    // The returned pointer remains valid for the lifetime of this snapshot.
    [[nodiscard]] const IndexedAsset* Find(std::wstring_view virtual_path) const noexcept;
    [[nodiscard]] const IndexedAsset* Find(const CanonicalVirtualPath& virtual_path) const noexcept;
    [[nodiscard]] std::span<const IndexedAsset> Assets() const noexcept { return assets_; }
    [[nodiscard]] std::span<const std::wstring> ModIds() const noexcept { return mod_ids_; }
    [[nodiscard]] const std::filesystem::path& ModsRoot() const noexcept { return mods_root_; }

private:
    friend struct ModIndexBuildResult;
    friend ModIndexBuildResult BuildModIndex(
        const std::filesystem::path&, const ModIndexOptions&);

    ModIndexSnapshot(
        std::filesystem::path mods_root,
        std::vector<std::wstring> mod_ids,
        std::vector<IndexedAsset> assets) noexcept;

    std::filesystem::path mods_root_;
    std::vector<std::wstring> mod_ids_;
    // Sorted by virtual_path.key, then mod_priority. Thus the first entry for
    // a key is the deterministic winner.
    std::vector<IndexedAsset> assets_;
};

struct ModIndexBuildResult final {
    std::shared_ptr<const ModIndexSnapshot> snapshot;
    ModIndexError error{ModIndexError::none};
    unsigned long system_error{};
    std::wstring detail;
    std::vector<ModIndexIssue> issues;
    std::size_t dropped_issue_count{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot != nullptr && error == ModIndexError::none;
    }
};

// Each immediate child directory is one mod. Mod ids are ordered by canonical
// lexical key; filesystem enumeration order never affects priority. Reparse
// points and invalid assets are reported and omitted from the snapshot. Entry,
// asset, aggregate-byte and depth limits fail the whole build closed. Stored
// diagnostics are capped independently; dropped_issue_count reports how many
// additional issues could not be retained.
[[nodiscard]] ModIndexBuildResult BuildModIndex(
    const std::filesystem::path& mods_root,
    const ModIndexOptions& options = {});

[[nodiscard]] std::wstring_view ModIndexErrorName(ModIndexError error) noexcept;
[[nodiscard]] std::wstring_view ModIndexIssueCodeName(ModIndexIssueCode code) noexcept;

}  // namespace ds2::modding
