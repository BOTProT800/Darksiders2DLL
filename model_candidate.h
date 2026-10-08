#pragma once
#include "mod_index.h"
#include "model_validation.h"
#include "package_identity_catalog.h"

namespace ds2::modding {
struct ModelRange final {
    std::uint32_t offset{}, size{};
    Sha256Digest original_hash{}, replacement_hash{};
};
struct ModelCandidate final {
    PackageResourceIdentity identity;
    CanonicalVirtualPath virtual_path;
    std::wstring mod_id;
    std::shared_ptr<const ByteStorage> storage; // original layout, also after a deletion
    std::vector<ModelRange> ranges;
    std::uint64_t mod_bytes{};
    std::uint32_t removed_vertices{}, removed_triangles{};
};
struct ModelCandidateSnapshot final {
    std::vector<ModelCandidate> entries;
    [[nodiscard]] const ModelCandidate* Find(const PackageResourceIdentity& identity) const noexcept;
};
struct ModelCandidateBuildResult final {
    std::shared_ptr<const ModelCandidateSnapshot> snapshot;
    std::vector<std::wstring> issues;
    std::wstring error;
};
[[nodiscard]] ModelCandidateBuildResult BuildModelCandidates(
    const std::filesystem::path& game_directory,
    const ModIndexSnapshot& mods,
    ModelEditContract contract = ModelEditContract::bounded_shape);
// Candidate construction is also used by the offline/native-model preflight.
[[nodiscard]] bool PrepareModelCandidate(
    const PackageIdentityEntry& entry, const IndexedAsset& asset,
    std::span<const std::byte> original, ModelCandidate& candidate, std::wstring& error,
    ModelEditContract contract = ModelEditContract::bounded_shape);
[[nodiscard]] bool IsNativeModelPath(std::wstring_view key) noexcept;
[[nodiscard]] const ModelRange* MatchModelRange(
    const ModelCandidate& candidate, std::uint32_t size, const Sha256Digest& source) noexcept;
}
