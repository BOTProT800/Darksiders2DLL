#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ds2::modding {
enum class ModelEditContract { positions_only, bounded_shape };
struct ModelVertexBlock final {
    std::uint32_t offset{}, size{}, vertex_count{}, stride{}, position_offset{};
    std::uint32_t index_offset{}, index_count{};
};
struct ModelLayout final {
    std::uint32_t data_offset{};
    std::vector<ModelVertexBlock> vertex_blocks;
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
}
