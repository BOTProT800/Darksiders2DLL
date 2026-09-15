#include "mod_index.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace ds2::modding {
namespace {

struct ModCandidate final {
    std::wstring id;
    std::wstring key;
    std::filesystem::path path;
};

struct BuildBudget final {
    std::size_t entry_count{};
    std::size_t asset_count{};
    std::uint64_t total_bytes{};
};

struct CanonicalAssetState final {
    std::optional<IndexedAsset> asset;
};

template <typename Unsigned>
bool CheckedAdd(
    const Unsigned left,
    const Unsigned right,
    Unsigned& sum) noexcept {
    static_assert((std::numeric_limits<Unsigned>::is_integer));
    static_assert(!(std::numeric_limits<Unsigned>::is_signed));
    if (right > (std::numeric_limits<Unsigned>::max)() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

bool HasReparsePoint(const std::filesystem::path& path, DWORD& error) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsDirectory(const std::filesystem::path& path, DWORD& error) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void AddIssue(
    ModIndexBuildResult& result,
    const std::size_t max_issue_count,
    const ModIndexIssueCode code,
    std::wstring mod_id,
    std::filesystem::path path,
    const unsigned long system_error,
    std::wstring detail) {
    if (result.issues.size() >= max_issue_count) {
        std::size_t next_dropped = 0;
        if (CheckedAdd(result.dropped_issue_count, std::size_t{1}, next_dropped)) {
            result.dropped_issue_count = next_dropped;
        } else {
            result.dropped_issue_count = (std::numeric_limits<std::size_t>::max)();
        }
        return;
    }
    result.issues.push_back(ModIndexIssue{
        code, std::move(mod_id), std::move(path), system_error, std::move(detail)});
}

bool FailResourceLimit(
    ModIndexBuildResult& result,
    const ModIndexOptions& options,
    std::wstring mod_id,
    std::filesystem::path path,
    std::wstring detail) {
    AddIssue(
        result, options.max_issue_count, ModIndexIssueCode::resource_limit_exceeded,
        std::move(mod_id), std::move(path), ERROR_NOT_ENOUGH_QUOTA, detail);
    result.snapshot.reset();
    result.error = ModIndexError::resource_limit_exceeded;
    result.system_error = ERROR_NOT_ENOUGH_QUOTA;
    result.detail = std::move(detail);
    return false;
}

bool ConsumeEntry(
    BuildBudget& budget,
    const ModIndexOptions& options,
    ModIndexBuildResult& result,
    const std::wstring_view mod_id,
    const std::filesystem::path& path) {
    std::size_t next_count = 0;
    if (!CheckedAdd(budget.entry_count, std::size_t{1}, next_count) ||
        next_count > options.max_entry_count) {
        return FailResourceLimit(
            result, options, std::wstring(mod_id), path,
            L"mod index entry-count limit exceeded");
    }
    budget.entry_count = next_count;
    return true;
}

bool ConsumeAsset(
    BuildBudget& budget,
    const ModIndexOptions& options,
    ModIndexBuildResult& result,
    const std::wstring_view mod_id,
    const std::filesystem::path& path) {
    std::size_t next_asset_count = 0;
    if (!CheckedAdd(budget.asset_count, std::size_t{1}, next_asset_count) ||
        next_asset_count > options.max_asset_count) {
        return FailResourceLimit(
            result, options, std::wstring(mod_id), path,
            L"mod index asset-count limit exceeded");
    }
    budget.asset_count = next_asset_count;
    return true;
}

bool ConsumeBytes(
    BuildBudget& budget,
    const std::uint64_t byte_count,
    const ModIndexOptions& options,
    ModIndexBuildResult& result,
    const std::wstring_view mod_id,
    const std::filesystem::path& path) {
    std::uint64_t next_total_bytes = 0;
    if (!CheckedAdd(budget.total_bytes, byte_count, next_total_bytes) ||
        next_total_bytes > options.max_total_bytes) {
        return FailResourceLimit(
            result, options, std::wstring(mod_id), path,
            L"mod index aggregate-byte limit exceeded");
    }
    budget.total_bytes = next_total_bytes;
    return true;
}

bool EndsWithDds(const std::wstring_view key) noexcept {
    constexpr std::wstring_view suffix = L".dds";
    return key.size() >= suffix.size() && key.substr(key.size() - suffix.size()) == suffix;
}

bool AssetLess(const IndexedAsset& left, const IndexedAsset& right) noexcept {
    if (left.virtual_path.key != right.virtual_path.key) {
        return left.virtual_path.key < right.virtual_path.key;
    }
    if (left.mod_priority != right.mod_priority) {
        return left.mod_priority < right.mod_priority;
    }
    return left.source_path.native() < right.source_path.native();
}

bool IndexMod(
    const ModCandidate& mod,
    const std::size_t priority,
    const ModIndexOptions& options,
    ModIndexBuildResult& result,
    BuildBudget& budget,
    std::vector<IndexedAsset>& assets) {
    std::error_code iterator_error;
    std::filesystem::recursive_directory_iterator iterator(
        mod.path, std::filesystem::directory_options::none, iterator_error);
    const std::filesystem::recursive_directory_iterator end;
    if (iterator_error) {
        AddIssue(result, options.max_issue_count, ModIndexIssueCode::filesystem_error, mod.id, mod.path,
                 static_cast<unsigned long>(iterator_error.value()), L"cannot enumerate mod root");
        return true;
    }

    std::map<std::wstring, CanonicalAssetState> canonical_assets;
    while (iterator != end) {
        const auto entry_path = iterator->path();
        if (!ConsumeEntry(budget, options, result, mod.id, entry_path)) {
            return false;
        }

        const int raw_depth = iterator.depth();
        std::size_t entry_depth = 0;
        if (raw_depth < 0 ||
            !CheckedAdd(static_cast<std::size_t>(raw_depth), std::size_t{1}, entry_depth) ||
            entry_depth > options.max_depth) {
            return FailResourceLimit(
                result, options, mod.id, entry_path,
                L"mod index directory-depth limit exceeded");
        }

        DWORD attribute_error = ERROR_SUCCESS;
        const bool is_reparse = HasReparsePoint(entry_path, attribute_error);
        if (attribute_error != ERROR_SUCCESS) {
            AddIssue(result, options.max_issue_count, ModIndexIssueCode::filesystem_error, mod.id, entry_path,
                     attribute_error, L"cannot inspect entry attributes");
            iterator.disable_recursion_pending();
        } else if (is_reparse) {
            AddIssue(result, options.max_issue_count, ModIndexIssueCode::reparse_point, mod.id, entry_path,
                     ERROR_REPARSE_TAG_INVALID, L"reparse-point entry rejected");
            std::error_code type_error;
            if (iterator->is_directory(type_error) && !type_error) {
                iterator.disable_recursion_pending();
            }
        } else {
            std::error_code type_error;
            const bool directory = iterator->is_directory(type_error);
            if (type_error) {
                AddIssue(result, options.max_issue_count, ModIndexIssueCode::filesystem_error, mod.id, entry_path,
                         static_cast<unsigned long>(type_error.value()), L"cannot determine entry type");
                iterator.disable_recursion_pending();
            } else {
                const auto relative = entry_path.lexically_relative(mod.path);
                const auto normalized = NormalizeVirtualPath(relative.generic_wstring());
                if (!normalized) {
                    AddIssue(result, options.max_issue_count, ModIndexIssueCode::unsafe_virtual_path, mod.id, entry_path,
                             ERROR_INVALID_NAME,
                             std::wstring(L"unsafe virtual path: ") +
                                 std::wstring(VirtualPathErrorName(normalized.error)));
                    if (directory) {
                        iterator.disable_recursion_pending();
                    }
                } else if (!directory && (!options.dds_only || EndsWithDds(normalized.path->key))) {
                    const bool regular = iterator->is_regular_file(type_error);
                    if (type_error || !regular) {
                        AddIssue(result, options.max_issue_count, ModIndexIssueCode::not_regular_file, mod.id, entry_path,
                                 type_error ? static_cast<unsigned long>(type_error.value()) : ERROR_INVALID_DATA,
                                 L"entry is not a regular file");
                    } else {
                        if (!ConsumeAsset(
                                budget, options, result, mod.id, entry_path)) {
                            return false;
                        }
                        const auto [state, inserted] =
                            canonical_assets.try_emplace(normalized.path->key);
                        if (!inserted) {
                            state->second.asset.reset();
                            AddIssue(
                                result, options.max_issue_count,
                                ModIndexIssueCode::duplicate_asset_in_mod,
                                mod.id, entry_path, ERROR_ALREADY_EXISTS,
                                L"ambiguous canonical asset path; all aliases rejected");
                            iterator.increment(iterator_error);
                            if (iterator_error) {
                                AddIssue(
                                    result, options.max_issue_count,
                                    ModIndexIssueCode::filesystem_error,
                                    mod.id, entry_path,
                                    static_cast<unsigned long>(iterator_error.value()),
                                    L"directory enumeration failed");
                                break;
                            }
                            continue;
                        }

                        VirtualPathError resolution_error = VirtualPathError::none;
                        const auto resolved = ResolveUnderRoot(mod.path, *normalized.path, &resolution_error);
                        if (!resolved) {
                            AddIssue(result, options.max_issue_count, ModIndexIssueCode::unsafe_virtual_path, mod.id, entry_path,
                                     ERROR_ACCESS_DENIED,
                                     std::wstring(L"path containment failed: ") +
                                         std::wstring(VirtualPathErrorName(resolution_error)));
                        } else {
                            const std::uint64_t remaining_total =
                                options.max_total_bytes - budget.total_bytes;
                            if (remaining_total == 0) {
                                return FailResourceLimit(
                                    result, options, mod.id, *resolved,
                                    L"mod index aggregate-byte limit exceeded");
                            }
                            const std::uint64_t effective_file_limit =
                                (std::min)(options.max_asset_file_size, remaining_total);
                            auto loaded = LoadByteStorageUnderRoot(
                                mod.path, *resolved, effective_file_limit);
                            if (!loaded) {
                                if (loaded.error == ByteLoadError::allocation_failed) {
                                    result.error = ModIndexError::allocation_failed;
                                    result.system_error = loaded.win32_error;
                                    result.detail = L"asset buffer allocation failed";
                                    return false;
                                }
                                if (loaded.error == ByteLoadError::file_too_large &&
                                    remaining_total < options.max_asset_file_size) {
                                    return FailResourceLimit(
                                        result, options, mod.id, *resolved,
                                        L"mod index aggregate-byte limit exceeded");
                                }
                                AddIssue(result, options.max_issue_count, ModIndexIssueCode::byte_load_failed, mod.id, *resolved,
                                         loaded.win32_error,
                                         std::wstring(ByteLoadErrorName(loaded.error)) + L": " + loaded.detail);
                            } else {
                                static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
                                const auto loaded_size =
                                    static_cast<std::uint64_t>(loaded.storage->Size());
                                if (!ConsumeBytes(
                                        budget, loaded_size, options, result,
                                        mod.id, *resolved)) {
                                    return false;
                                }
                                std::optional<DdsMetadata> dds;
                                if (EndsWithDds(normalized.path->key)) {
                                    auto validation = ValidateDds(loaded.storage->Bytes());
                                    if (!validation) {
                                        AddIssue(result, options.max_issue_count, ModIndexIssueCode::invalid_dds, mod.id, *resolved,
                                                 ERROR_INVALID_DATA,
                                                 std::wstring(DdsValidationErrorName(validation.error)) +
                                                     L": " + validation.detail);
                                    } else {
                                        dds = *validation.metadata;
                                    }
                                }
                                if (!EndsWithDds(normalized.path->key) || dds.has_value()) {
                                    state->second.asset.emplace(IndexedAsset{
                                        mod.id, mod.key, priority, std::move(*normalized.path),
                                        *resolved, std::move(loaded.storage), std::move(dds)});
                                }
                            }
                        }
                    }
                }
            }
        }
        iterator.increment(iterator_error);
        if (iterator_error) {
            AddIssue(result, options.max_issue_count, ModIndexIssueCode::filesystem_error, mod.id, entry_path,
                     static_cast<unsigned long>(iterator_error.value()), L"directory enumeration failed");
            // The standard does not guarantee that a failed recursive
            // increment advances. Stop this mod instead of risking a tight
            // loop over the same unreadable entry.
            break;
        }
    }
    for (auto& entry : canonical_assets) {
        if (entry.second.asset.has_value()) {
            assets.push_back(std::move(*entry.second.asset));
        }
    }
    return true;
}

}  // namespace

ModIndexSnapshot::ModIndexSnapshot(
    std::filesystem::path mods_root,
    std::vector<std::wstring> mod_ids,
    std::vector<IndexedAsset> assets) noexcept
    : mods_root_(std::move(mods_root)),
      mod_ids_(std::move(mod_ids)),
      assets_(std::move(assets)) {}

const IndexedAsset* ModIndexSnapshot::Find(const std::wstring_view virtual_path) const noexcept {
    try {
        const auto normalized = NormalizeVirtualPath(virtual_path);
        return normalized ? Find(*normalized.path) : nullptr;
    } catch (...) {
        return nullptr;
    }
}

const IndexedAsset* ModIndexSnapshot::Find(
    const CanonicalVirtualPath& virtual_path) const noexcept {
    const auto found = std::lower_bound(
        assets_.begin(), assets_.end(), virtual_path.key,
        [](const IndexedAsset& asset, const std::wstring_view key) {
            return asset.virtual_path.key < key;
        });
    if (found == assets_.end() || found->virtual_path.key != virtual_path.key) {
        return nullptr;
    }
    return &*found;
}

ModIndexBuildResult BuildModIndex(
    const std::filesystem::path& mods_root,
    const ModIndexOptions& options) {
    ModIndexBuildResult result;
    if (mods_root.empty() || options.max_asset_file_size == 0 ||
        options.max_total_bytes == 0 || options.max_asset_count == 0 ||
        options.max_entry_count == 0 || options.max_depth == 0) {
        result.error = ModIndexError::invalid_root;
        result.system_error = ERROR_INVALID_PARAMETER;
        result.detail = L"invalid mods root or resource limit";
        return result;
    }

    try {
        std::error_code filesystem_error;
        auto absolute_root = std::filesystem::absolute(mods_root, filesystem_error);
        if (filesystem_error) {
            result.error = ModIndexError::filesystem_error;
            result.system_error = static_cast<unsigned long>(filesystem_error.value());
            result.detail = L"cannot make mods root absolute";
            return result;
        }

        const auto status = std::filesystem::symlink_status(absolute_root, filesystem_error);
        if (filesystem_error == std::errc::no_such_file_or_directory ||
            status.type() == std::filesystem::file_type::not_found) {
            result.snapshot = std::shared_ptr<const ModIndexSnapshot>(
                new ModIndexSnapshot(std::move(absolute_root), {}, {}));
            return result;
        }
        if (filesystem_error || !std::filesystem::is_directory(status)) {
            result.error = ModIndexError::filesystem_error;
            result.system_error = filesystem_error
                ? static_cast<unsigned long>(filesystem_error.value()) : ERROR_DIRECTORY;
            result.detail = L"mods root is not an accessible directory";
            return result;
        }
        DWORD attribute_error = ERROR_SUCCESS;
        if (HasReparsePoint(absolute_root, attribute_error)) {
            result.error = ModIndexError::root_reparse_point;
            result.system_error = ERROR_REPARSE_TAG_INVALID;
            result.detail = L"mods root is a reparse point";
            return result;
        }
        if (attribute_error != ERROR_SUCCESS) {
            result.error = ModIndexError::filesystem_error;
            result.system_error = attribute_error;
            result.detail = L"cannot inspect mods root";
            return result;
        }
        absolute_root = std::filesystem::weakly_canonical(absolute_root, filesystem_error);
        if (filesystem_error) {
            result.error = ModIndexError::filesystem_error;
            result.system_error = static_cast<unsigned long>(filesystem_error.value());
            result.detail = L"cannot canonicalize mods root";
            return result;
        }

        BuildBudget budget;
        std::vector<ModCandidate> mods;
        std::filesystem::directory_iterator iterator(absolute_root, filesystem_error);
        const std::filesystem::directory_iterator end;
        while (!filesystem_error && iterator != end) {
            const auto path = iterator->path();
            if (!ConsumeEntry(budget, options, result, {}, path)) {
                return result;
            }
            DWORD entry_error = ERROR_SUCCESS;
            const bool reparse = HasReparsePoint(path, entry_error);
            if (entry_error != ERROR_SUCCESS) {
                AddIssue(result, options.max_issue_count, ModIndexIssueCode::filesystem_error, {}, path, entry_error,
                         L"cannot inspect candidate mod");
            } else if (reparse) {
                AddIssue(result, options.max_issue_count, ModIndexIssueCode::reparse_point, path.filename().wstring(), path,
                         ERROR_REPARSE_TAG_INVALID, L"reparse-point mod directory rejected");
            } else {
                const bool directory = IsDirectory(path, entry_error);
                if (entry_error != ERROR_SUCCESS) {
                    AddIssue(result, options.max_issue_count, ModIndexIssueCode::filesystem_error, {}, path, entry_error,
                             L"cannot inspect candidate mod type");
                } else if (directory) {
                    const auto id = path.filename().wstring();
                    const auto normalized = NormalizeVirtualPath(id);
                    if (!normalized || normalized.path->display.find(L'/') != std::wstring::npos) {
                        AddIssue(result, options.max_issue_count, ModIndexIssueCode::unsafe_mod_id, id, path, ERROR_INVALID_NAME,
                                 L"unsafe mod id");
                    } else {
                        mods.push_back(ModCandidate{id, normalized.path->key, path});
                    }
                }
            }
            iterator.increment(filesystem_error);
        }
        if (filesystem_error) {
            result.error = ModIndexError::filesystem_error;
            result.system_error = static_cast<unsigned long>(filesystem_error.value());
            result.detail = L"cannot enumerate mods root";
            return result;
        }

        std::sort(mods.begin(), mods.end(), [](const ModCandidate& left, const ModCandidate& right) {
            return left.key != right.key ? left.key < right.key : left.id < right.id;
        });
        std::vector<ModCandidate> unique_mods;
        for (std::size_t group_begin = 0; group_begin < mods.size();) {
            std::size_t group_end = group_begin + 1;
            while (group_end < mods.size() &&
                   mods[group_end].key == mods[group_begin].key) {
                ++group_end;
            }
            if (group_end - group_begin == 1) {
                unique_mods.push_back(std::move(mods[group_begin]));
            } else {
                for (std::size_t index = group_begin; index < group_end; ++index) {
                    AddIssue(
                        result, options.max_issue_count,
                        ModIndexIssueCode::duplicate_mod_id,
                        mods[index].id, mods[index].path, ERROR_ALREADY_EXISTS,
                        L"ambiguous canonical mod id; all aliases rejected");
                }
            }
            group_begin = group_end;
        }

        std::vector<std::wstring> mod_ids;
        std::vector<IndexedAsset> assets;
        mod_ids.reserve(unique_mods.size());
        for (std::size_t priority = 0; priority < unique_mods.size(); ++priority) {
            mod_ids.push_back(unique_mods[priority].id);
            if (!IndexMod(
                    unique_mods[priority], priority, options,
                    result, budget, assets)) {
                return result;
            }
        }
        std::sort(assets.begin(), assets.end(), AssetLess);
        result.snapshot = std::shared_ptr<const ModIndexSnapshot>(new ModIndexSnapshot(
            std::move(absolute_root), std::move(mod_ids), std::move(assets)));
        return result;
    } catch (const std::bad_alloc&) {
        result.snapshot.reset();
        result.error = ModIndexError::allocation_failed;
        result.system_error = ERROR_NOT_ENOUGH_MEMORY;
        result.detail = L"mod index allocation failed";
        return result;
    } catch (const std::filesystem::filesystem_error& error) {
        result.snapshot.reset();
        result.error = ModIndexError::filesystem_error;
        result.system_error = static_cast<unsigned long>(error.code().value());
        result.detail = L"unexpected filesystem error";
        return result;
    }
}

std::wstring_view ModIndexErrorName(const ModIndexError error) noexcept {
    switch (error) {
    case ModIndexError::none: return L"none";
    case ModIndexError::invalid_root: return L"invalid_root";
    case ModIndexError::root_reparse_point: return L"root_reparse_point";
    case ModIndexError::filesystem_error: return L"filesystem_error";
    case ModIndexError::resource_limit_exceeded: return L"resource_limit_exceeded";
    case ModIndexError::allocation_failed: return L"allocation_failed";
    }
    return L"unknown";
}

std::wstring_view ModIndexIssueCodeName(const ModIndexIssueCode code) noexcept {
    switch (code) {
    case ModIndexIssueCode::unsafe_mod_id: return L"unsafe_mod_id";
    case ModIndexIssueCode::duplicate_mod_id: return L"duplicate_mod_id";
    case ModIndexIssueCode::reparse_point: return L"reparse_point";
    case ModIndexIssueCode::unsafe_virtual_path: return L"unsafe_virtual_path";
    case ModIndexIssueCode::filesystem_error: return L"filesystem_error";
    case ModIndexIssueCode::not_regular_file: return L"not_regular_file";
    case ModIndexIssueCode::byte_load_failed: return L"byte_load_failed";
    case ModIndexIssueCode::invalid_dds: return L"invalid_dds";
    case ModIndexIssueCode::duplicate_asset_in_mod: return L"duplicate_asset_in_mod";
    case ModIndexIssueCode::resource_limit_exceeded: return L"resource_limit_exceeded";
    }
    return L"unknown";
}

}  // namespace ds2::modding
