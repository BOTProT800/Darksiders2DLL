#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ds2::modding {
// unchecked: no content rule at all (geometry, UV, skin, bounds, bones); only
// the original read sizes, which the engine takes from the packaged member.
enum class ModelEditContract { positions_only, bounded_shape, unchecked };
struct ModelVertexBlock final {
    std::uint32_t offset{}, size{}, vertex_count{}, stride{}, position_offset{};
    std::uint32_t index_offset{}, index_count{};
};
struct ModelRead final {
    std::uint32_t offset{}, size{};
    bool operator==(const ModelRead&) const = default;
};
struct ModelLayout final {
    std::uint32_t data_offset{};
    std::vector<ModelVertexBlock> vertex_blocks;
    // Each array the engine reads on its own: header, then per mesh the index
    // buffer (after its 8-byte prefix) and every per-vertex array.
    std::vector<ModelRead> reads;
};
struct ModelValidationResult final {
    ModelLayout layout;
    std::wstring error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// Both contracts keep headers (including bounds), topology, UVs and skinning
// byte-identical, with finite positions inside the original model-space AABB.
// bounded_shape additionally permits the verified skinned normal/tangent layout
// and removed triangles collapsed in place to (a,a,a), with a taken from the
// original triangle; static vertex data is immutable in that contract. No
// descriptor-space box edits.
[[nodiscard]] ModelValidationResult ValidateModelEdit(
    std::span<const std::byte> original, std::span<const std::byte> replacement,
    ModelEditContract contract);
[[nodiscard]] ModelValidationResult ValidateModelPositionEdit(
    std::span<const std::byte> original, std::span<const std::byte> replacement);

struct ModelExpansionResult final {
    std::vector<std::byte> bytes;
    std::uint32_t removed_vertices{}, removed_triangles{};
    std::wstring error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// The engine reads a member with the sizes of the packaged original, so a
// smaller .2 cannot be delivered as is. A pure deletion (Anansi prune_dcm:
// surviving records copied whole, header counts/spans/offsets recomputed) is
// rebuilt in the original layout: header and unreferenced records stay
// original, each surviving vertex record goes to the slot of the original
// triangle corner it matches, and removed triangles collapse in place. The
// result renders the pruned triangles with the pruned vertex data; validate it
// with ValidateModelEdit(bounded_shape) like any same-size edit.
[[nodiscard]] ModelExpansionResult ExpandModelDeletion(
    std::span<const std::byte> original, std::span<const std::byte> pruned);

struct ModelFitResult final {
    std::vector<std::byte> bytes;
    std::uint32_t spare_vertices{}, collapsed_triangles{};
    std::wstring error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// Fits any replacement .2 (new mesh, UV, skin, bounds or bones) into the read
// sizes of the original: the header comes from the replacement with the
// original counts, spans and offsets; each material's triangles and vertices
// go to the start of the original span, and the rest of the span collapses to
// (a,a,a) over original records. Only the physical limits are enforced: same
// header size, mesh count, mesh kinds and material count, and no mesh or
// material larger than the original one. No content check is made.
[[nodiscard]] ModelFitResult FitModelReplacement(
    std::span<const std::byte> original, std::span<const std::byte> replacement);

// The counts the mesh cache reads from the package metadata (D3D9 0x75908C)
// and repeats in the .2 header: header size, then per mesh the index count,
// the signed vertex count (negative = static) and, for the first `pairs` meshes,
// the integer of the header's pair table.
struct ModelEngineMesh final {
    std::uint32_t indices{};
    std::int32_t vertices{}, pair{};
};
struct ModelEngineCounts final {
    std::uint32_t header_size{}, pairs{};
    std::vector<ModelEngineMesh> meshes;
};
// One buffer of the engine's read list; skipped buffers have a null destination.
struct ModelEngineBuffer final {
    std::uint32_t size{};
    bool skipped{};
    bool operator==(const ModelEngineBuffer&) const = default;
};
[[nodiscard]] bool ReadModelEngineCounts(std::span<const std::byte> model, ModelEngineCounts& counts) noexcept;
[[nodiscard]] std::vector<ModelEngineBuffer> ModelEngineBuffers(const ModelEngineCounts& counts);
[[nodiscard]] std::uint64_t ModelEngineSize(const ModelEngineCounts& counts) noexcept;
[[nodiscard]] std::vector<std::byte> EncodeModelEngineExtra(const ModelEngineCounts& counts);

struct ModelResizePlan final {
    std::vector<std::byte> extra;            // replacement counts, same length as the original
    std::vector<ModelEngineBuffer> buffers;  // the engine's reads of the replacement, in order
    std::uint32_t original_header_size{}, replacement_header_size{};
    std::wstring error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// A replacement of any size is deliverable when the engine's buffer list,
// computed from its own counts, spans exactly its bytes, and its counts fit the
// original's metadata entry (same mesh and pair count). The original's entry
// must equal the counts derived from its header, proving the layout per member.
[[nodiscard]] ModelResizePlan PlanModelResize(
    std::span<const std::byte> original, std::span<const std::byte> replacement,
    std::span<const std::byte> original_extra);
}
