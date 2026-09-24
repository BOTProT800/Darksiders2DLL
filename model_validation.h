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
// bounded_shape additionally permits the verified skinned normal/tangent layout;
// static meshes are immutable in that contract. No descriptor-space box edits.
[[nodiscard]] ModelValidationResult ValidateModelEdit(
    std::span<const std::byte> original, std::span<const std::byte> replacement,
    ModelEditContract contract);
[[nodiscard]] ModelValidationResult ValidateModelPositionEdit(
    std::span<const std::byte> original, std::span<const std::byte> replacement);
}
