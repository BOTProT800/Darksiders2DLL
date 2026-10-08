#include "animation_candidate.h"
#include "package_member_bytes.h"
#include <algorithm>
#include <tuple>

namespace ds2::modding {
namespace {
constexpr std::uint32_t kAnimationTypeId = 8;
constexpr std::uint32_t kAnimationHeaderSize = 16;
constexpr std::uint64_t kMaxAnimationBytes = 64ull * 1024 * 1024;
auto Key(const PackageResourceIdentity& identity) noexcept {
    return std::tuple(identity.package_base, identity.member_table_offset, identity.member_ordinal);
}
}
bool IsNativeAnimationPath(const std::wstring_view key) noexcept { return key.ends_with(L".anm"); }
const AnimationCandidate* AnimationCandidateSnapshot::Find(const PackageResourceIdentity& identity) const noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), identity,
        [](const AnimationCandidate& entry, const auto& id) { return Key(entry.identity) < Key(id); });
    return it != entries.end() && it->identity == identity ? &*it : nullptr;
}
const AnimationRange* MatchAnimationRange(const AnimationCandidate& candidate, const std::uint32_t size,
                                          const Sha256Digest& source) noexcept {
    const AnimationRange* found = nullptr;
    for (const auto& range : candidate.ranges) {
        if (range.size == size && range.original_hash == source) {
            if (found != nullptr) return nullptr; // ambiguous range: never guess
            found = &range;
        }
    }
    return found;
}
bool PrepareAnimationCandidate(const PackageIdentityEntry& entry, const IndexedAsset& asset,
                               const std::span<const std::byte> original,
                               AnimationCandidate& candidate, std::wstring& error,
                               const AnimationEditContract contract) {
    candidate = {};
    error.clear();
    if (entry.type_id != kAnimationTypeId || !IsNativeAnimationPath(entry.virtual_path.key) ||
        asset.virtual_path.key != entry.virtual_path.key || !asset.storage ||
        original.size() != entry.original_size || original.size() <= kAnimationHeaderSize ||
        original.size() > kMaxAnimationBytes) {
        error = L"invalid native animation identity/storage";
        return false;
    }
    const auto replacement = asset.storage->Bytes();
    const auto validated = ValidateAnimationEdit(original, replacement, contract);
    if (!validated) { error = validated.error; return false; }
    AnimationCandidate prepared{entry.identity, entry.virtual_path, asset.mod_id, asset.storage,
                                validated.layout, validated.changed_tracks, {}};
    const auto add_range = [&](const std::uint32_t offset, const std::uint32_t size) {
        const auto before = ComputeSha256(original.subspan(offset, size));
        const auto after = ComputeSha256(replacement.subspan(offset, size));
        if (!before || !after) return false;
        prepared.ranges.push_back({offset, size, before.digest, after.digest});
        return true;
    };
    if (!add_range(0, entry.original_size) || !add_range(0, kAnimationHeaderSize) ||
        !add_range(kAnimationHeaderSize, entry.original_size - kAnimationHeaderSize)) {
        error = L"animation range hashing failed";
        return false;
    }
    candidate = std::move(prepared);
    return true;
}
AnimationCandidateBuildResult BuildAnimationCandidates(const std::filesystem::path& game_directory,
                                                       const ModIndexSnapshot& mods,
                                                       const AnimationEditContract contract) {
    AnimationCandidateBuildResult result;
    try {
        std::vector<CanonicalVirtualPath> paths;
        for (const auto& asset : mods.Assets())
            if (IsNativeAnimationPath(asset.virtual_path.key)) paths.push_back(asset.virtual_path);
        auto snapshot = std::make_shared<AnimationCandidateSnapshot>();
        if (paths.empty()) { result.snapshot = snapshot; return result; }
        // Every installed clip lives in a single-stream segment; per-member
        // block layouts stay fail-closed for animations.
        const auto catalog = BuildMediaPackageIdentityCatalog(game_directory, paths);
        if (!catalog) { result.error = catalog.detail; return result; }
        for (const auto& issue : catalog.issues)
            result.issues.push_back(issue.virtual_path + L": " + issue.detail);
        std::vector<PackageIdentityEntry> requested;
        for (const auto& entry : catalog.snapshot->Entries()) {
            if (entry.type_id == kAnimationTypeId) requested.push_back(entry);
            else result.issues.push_back(entry.virtual_path.key + L": not a native animation member");
        }
        PackageDdsContractCatalogOptions limits;
        limits.max_asset_bytes = kMaxAnimationBytes;
        limits.max_total_asset_bytes = 256ull * 1024 * 1024;
        const auto originals = ReadMediaPackageMembers(game_directory, requested, limits);
        if (!originals) {
            result.error = L"original animation extraction failed: " + originals.diagnostics.detail;
            return result;
        }
        for (const auto& original : originals.members) {
            const auto* entry = catalog.snapshot->Find(original.identity);
            const auto* asset = entry ? mods.Find(entry->virtual_path) : nullptr;
            if (!entry || !asset || !original.bytes) { result.error = L"animation join failed"; return result; }
            AnimationCandidate candidate;
            std::wstring issue;
            if (PrepareAnimationCandidate(*entry, *asset, *original.bytes, candidate, issue, contract))
                snapshot->entries.push_back(std::move(candidate));
            else result.issues.push_back(entry->virtual_path.key + L": " + issue);
        }
        std::sort(snapshot->entries.begin(), snapshot->entries.end(),
            [](const auto& a, const auto& b) { return Key(a.identity) < Key(b.identity); });
        result.snapshot = std::move(snapshot);
    } catch (...) { result.error = L"animation candidate preparation failed"; }
    return result;
}
}
