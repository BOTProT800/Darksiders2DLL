#include "model_candidate.h"
#include "model_validation.h"
#include "package_member_bytes.h"
#include <algorithm>
#include <tuple>

namespace ds2::modding {
namespace {
auto Key(const PackageResourceIdentity& identity) noexcept {
    return std::tuple(identity.package_base, identity.member_table_offset, identity.member_ordinal);
}
}
bool IsNativeModelPath(const std::wstring_view key) noexcept { return key.ends_with(L".2"); }
const ModelCandidate* ModelCandidateSnapshot::Find(const PackageResourceIdentity& identity) const noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), identity,
        [](const ModelCandidate& entry, const auto& id) { return Key(entry.identity) < Key(id); });
    return it != entries.end() && it->identity == identity ? &*it : nullptr;
}
const ModelRange* MatchModelRange(const ModelCandidate& candidate, const std::uint32_t size,
                                const Sha256Digest& source) noexcept {
    const ModelRange* found = nullptr;
    for (const auto& range : candidate.ranges) {
        if (range.size == size && range.original_hash == source) {
            if (found != nullptr) return nullptr; // ambiguous submesh: never guess
            found = &range;
        }
    }
    return found;
}
bool PrepareModelCandidate(const PackageIdentityEntry& entry, const IndexedAsset& asset,
                           const std::span<const std::byte> original,
                           ModelCandidate& candidate, std::wstring& error,
                           const ModelEditContract contract) {
    candidate = {};
    error.clear();
    if (entry.type_id != 2 || !IsNativeModelPath(entry.virtual_path.key) ||
        asset.virtual_path.key != entry.virtual_path.key || !asset.storage ||
        original.size() != entry.original_size || original.empty() ||
        original.size() > 64ull * 1024 * 1024) {
        error = L"invalid native model identity/storage";
        return false;
    }
    auto storage = asset.storage;
    std::uint32_t removed_vertices = 0, removed_triangles = 0;
    if (contract == ModelEditContract::bounded_shape && storage->Size() < original.size()) {
        auto expanded = ExpandModelDeletion(original, storage->Bytes());
        if (!expanded) { error = L"deletion: " + expanded.error; return false; }
        removed_vertices = expanded.removed_vertices;
        removed_triangles = expanded.removed_triangles;
        auto rebuilt = MakeByteStorage(std::move(expanded.bytes));
        if (!rebuilt) { error = L"expanded model storage failed"; return false; }
        storage = std::move(rebuilt.storage);
    }
    const auto replacement = storage->Bytes();
    const auto validated = ValidateModelEdit(original, replacement, contract);
    if (!validated) { error = validated.error; return false; }
    ModelCandidate prepared{entry.identity, entry.virtual_path, asset.mod_id, storage, {},
                            asset.storage->Size(), removed_vertices, removed_triangles};
    const auto add_range = [&](const std::uint32_t offset, const std::uint32_t size) {
        if (size == 0 || offset > original.size() || size > original.size() - offset) return false;
        for (const auto& range : prepared.ranges)
            if (range.offset == offset && range.size == size) return true;
        const auto before = ComputeSha256(original.subspan(offset, size));
        const auto after = ComputeSha256(replacement.subspan(offset, size));
        if (!before || !after) return false;
        prepared.ranges.push_back({offset, size, before.digest, after.digest});
        return true;
    };
    if (!add_range(0, entry.original_size) ||
        !add_range(validated.layout.data_offset, entry.original_size - validated.layout.data_offset)) {
        error = L"model range hashing failed"; return false;
    }
    for (const auto& block : validated.layout.vertex_blocks) {
        // The engine reads each index buffer on its own, after its 8-byte prefix.
        if (!add_range(block.index_offset, block.index_count * 2) || !add_range(block.offset, block.size)) {
            error = L"model vertex range hashing failed"; return false;
        }
    }
    candidate = std::move(prepared);
    return true;
}
ModelCandidateBuildResult BuildModelCandidates(const std::filesystem::path& game_directory,
                                               const ModIndexSnapshot& mods,
                                               const ModelEditContract contract) {
    ModelCandidateBuildResult result;
    try {
        std::vector<CanonicalVirtualPath> paths;
        for (const auto& asset : mods.Assets())
            if (IsNativeModelPath(asset.virtual_path.key)) paths.push_back(asset.virtual_path);
        auto snapshot = std::make_shared<ModelCandidateSnapshot>();
        if (paths.empty()) { result.snapshot = snapshot; return result; }
        PackageIdentityCatalogOptions identity_options;
        identity_options.allow_member_blocks = true;
        const auto catalog = BuildMediaPackageIdentityCatalog(game_directory, paths, identity_options);
        if (!catalog) { result.error = catalog.detail; return result; }
        for (const auto& issue : catalog.issues)
            result.issues.push_back(issue.virtual_path + L": " + issue.detail);
        std::vector<PackageIdentityEntry> requested;
        for (const auto& entry : catalog.snapshot->Entries()) {
            if (entry.type_id == 2) requested.push_back(entry);
            else result.issues.push_back(entry.virtual_path.key + L": not a native geometry member");
        }
        PackageDdsContractCatalogOptions limits;
        limits.max_asset_bytes = 64ull * 1024 * 1024;
        limits.max_total_asset_bytes = 256ull * 1024 * 1024;
        const auto originals = ReadMediaPackageMembers(game_directory, requested, limits);
        if (!originals) { result.error = L"original model extraction failed: " + originals.diagnostics.detail; return result; }
        for (const auto& original : originals.members) {
            const auto* entry = catalog.snapshot->Find(original.identity);
            const auto* asset = entry ? mods.Find(entry->virtual_path) : nullptr;
            if (!entry || !asset || !original.bytes) { result.error = L"model join failed"; return result; }
            ModelCandidate candidate;
            std::wstring issue;
            if (PrepareModelCandidate(*entry, *asset, *original.bytes, candidate, issue, contract))
                snapshot->entries.push_back(std::move(candidate));
            else result.issues.push_back(entry->virtual_path.key + L": " + issue);
        }
        std::sort(snapshot->entries.begin(), snapshot->entries.end(),
            [](const auto& a, const auto& b) { return Key(a.identity) < Key(b.identity); });
        result.snapshot = std::move(snapshot);
    } catch (...) { result.error = L"model candidate preparation failed"; }
    return result;
}
}
