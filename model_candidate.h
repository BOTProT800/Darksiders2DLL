#pragma once
#include "mod_index.h"
#include "model_validation.h"
#include "package_identity_catalog.h"
#include <optional>

namespace ds2::modding {
struct ModelRange final {
    std::uint32_t offset{}, size{};
    Sha256Digest original_hash{}, replacement_hash{};
};
// A replacement that does not fit the original read sizes: its counts are
// written over this member's entry in the package metadata block when the
// engine reads it, and the member's reads are then served from the
// replacement after the original has been consumed from the stream. A header
// request of the original size means the engine kept the original counts:
// the member is then read unchanged.
struct ModelResize final {
    std::uint32_t metadata_size{}, extra_offset{};
    Sha256Digest metadata_sha256{};
    std::vector<std::byte> extra;            // replacement counts, original length
    std::vector<ModelEngineBuffer> buffers;  // the engine's reads of the replacement
    std::uint32_t original_header_size{}, replacement_header_size{};
    std::uint32_t original_size{};
    Sha256Digest original_sha256{};
    std::vector<std::wstring> warnings;  // untested predictions, logged only
};
struct ModelCandidate final {
    PackageResourceIdentity identity;
    CanonicalVirtualPath virtual_path;
    std::wstring mod_id;
    std::shared_ptr<const ByteStorage> storage; // original layout, also after a deletion
    std::vector<ModelRange> ranges;
    std::uint64_t mod_bytes{};
    std::uint32_t removed_vertices{}, removed_triangles{};
    bool unchecked_fit{};
    std::uint32_t spare_vertices{}, collapsed_triangles{};
    std::optional<ModelResize> resize; // storage is the replacement as is
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
