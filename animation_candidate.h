#pragma once
#include "animation_validation.h"
#include "mod_index.h"
#include "package_identity_catalog.h"

namespace ds2::modding {
struct AnimationRange final {
    std::uint32_t offset{}, size{};
    Sha256Digest original_hash{}, replacement_hash{};
};
struct AnimationCandidate final {
    PackageResourceIdentity identity;
    CanonicalVirtualPath virtual_path;
    std::wstring mod_id;
    std::shared_ptr<const ByteStorage> storage;
    AnimationLayout layout;
    std::uint32_t changed_tracks{};
    // Whole clip, 16-byte header and body. The read hook only observes them;
    // which ranges the engine requests has not been confirmed in game yet.
    std::vector<AnimationRange> ranges;
};
struct AnimationCandidateSnapshot final {
    std::vector<AnimationCandidate> entries;
    [[nodiscard]] const AnimationCandidate* Find(const PackageResourceIdentity& identity) const noexcept;
};
struct AnimationCandidateBuildResult final {
    std::shared_ptr<const AnimationCandidateSnapshot> snapshot;
    std::vector<std::wstring> issues;
    std::wstring error;
};
[[nodiscard]] AnimationCandidateBuildResult BuildAnimationCandidates(
    const std::filesystem::path& game_directory,
    const ModIndexSnapshot& mods,
    AnimationEditContract contract = AnimationEditContract::keys_in_place);
[[nodiscard]] bool PrepareAnimationCandidate(
    const PackageIdentityEntry& entry, const IndexedAsset& asset,
    std::span<const std::byte> original, AnimationCandidate& candidate, std::wstring& error,
    AnimationEditContract contract = AnimationEditContract::keys_in_place);
[[nodiscard]] bool IsNativeAnimationPath(std::wstring_view key) noexcept;
[[nodiscard]] const AnimationRange* MatchAnimationRange(
    const AnimationCandidate& candidate, std::uint32_t size, const Sha256Digest& source) noexcept;
}
