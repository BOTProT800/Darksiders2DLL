#include "model_validation.h"
// Native .2 layout references: Anansi/services/darksiders/{mesh,native}.py,
// Copyright (c) 2026 BOTProT800, MIT. See THIRD_PARTY_NOTICES.md.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ds2::modding {
namespace {
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t cursor{};
    void Skip(const std::size_t n) {
        if (n > bytes.size() - cursor) throw std::runtime_error("truncated");
        cursor += n;
    }
    template<class T> T Read() {
        const auto start = cursor;
        Skip(sizeof(T));
        T value{};
        std::memcpy(&value, bytes.data() + start, sizeof(T));
        return value;
    }
    std::uint32_t Count(const bool absolute = false) {
        std::int64_t value = Read<std::int32_t>();
        if (absolute && value < 0) value = -value;
        if (value < 0 || value > 1'000'000) throw std::runtime_error("count");
        return static_cast<std::uint32_t>(value);
    }
};
float FloatAt(const std::span<const std::byte> bytes, const std::size_t offset) {
    float value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
void ValidateBasis(const std::span<const std::byte> bytes, const std::size_t vertex) {
    double normal_length = 0, tangent_length = 0, dot = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double n = FloatAt(bytes, vertex + 20 + axis * 4);
        const double t = FloatAt(bytes, vertex + 36 + axis * 4);
        if (!std::isfinite(n) || !std::isfinite(t))
            throw std::runtime_error("non-finite normal/tangent");
        normal_length += n * n;
        tangent_length += t * t;
        dot += n * t;
    }
    const double sign = FloatAt(bytes, vertex + 48);
    if (std::abs(std::sqrt(normal_length) - 1.0) > 0.0011 ||
        std::abs(std::sqrt(tangent_length) - 1.0) > 0.0011 ||
        std::abs(dot) > 0.0011 || !std::isfinite(sign) ||
        std::abs(std::abs(sign) - 1.0) > 0.0011)
        throw std::runtime_error("invalid orthonormal tangent basis");
    // This duplicate is a layout discriminator, not another editable scalar.
    if (FloatAt(bytes, vertex + 28) != FloatAt(bytes, vertex + 32))
        throw std::runtime_error("unverified duplicate normal.z layout");
}
ModelLayout Parse(const std::span<const std::byte> bytes) {
    if (bytes.size() < 32 || bytes.size() > 64ull * 1024 * 1024)
        throw std::runtime_error("size");
    Reader r{bytes};
    ModelLayout layout;
    layout.data_offset = r.Read<std::uint32_t>();
    const auto meshes = r.Count();
    if (meshes == 0 || meshes > 256 || layout.data_offset > bytes.size())
        throw std::runtime_error("header");
    if (r.Count() != meshes) throw std::runtime_error("index table");
    std::vector<std::uint32_t> indices(meshes), vertices(meshes);
    for (auto& n : indices) n = r.Count();
    if (r.Count(true) != meshes) throw std::runtime_error("vertex table");
    for (auto& n : vertices) n = r.Count(true);
    const auto extra = r.Count(true);
    r.Skip(static_cast<std::size_t>(extra) * 8);
    std::uint64_t previous_end = layout.data_offset;
    for (std::uint32_t i = 0; i < meshes; ++i) {
        const auto kind = r.Read<std::uint8_t>();
        const auto n = r.Count();
        const auto header_stride = r.Count();
        const auto relative = r.Count();
        const auto field3 = r.Count(), field4 = r.Count();
        if (kind > 1 || n == 0 || n != vertices[i] ||
            header_stride != (kind == 1 ? 64u : 60u) ||
            indices[i] == 0 || indices[i] % 3 != 0 || relative < 8)
            throw std::runtime_error("mesh layout");
        std::array<float, 6> bounds;
        for (auto& value : bounds) {
            value = r.Read<float>();
            if (!std::isfinite(value)) throw std::runtime_error("bounds");
        }
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (bounds[axis] > bounds[axis + 3]) throw std::runtime_error("inverted bounds");
        const auto bones = kind == 1 ? field4 : 0;
        const auto materials = kind == 1 ? field3 : field4;
        if (bones > 256 || materials > 4096) throw std::runtime_error("references");
        for (std::uint32_t b = 0; b < bones; ++b) {
            r.Skip(1);
            const auto length = r.Read<std::uint16_t>();
            if (length == 0 || length > 4096) throw std::runtime_error("bone name");
            r.Skip(length);
            for (int f = 0; f < 16; ++f)
                if (!std::isfinite(r.Read<float>())) throw std::runtime_error("bind matrix");
            r.Skip(4);
        }
        for (std::uint32_t m = 0; m < materials; ++m) {
            const bool named = r.Read<std::uint8_t>() == 255;
            if (named) {
                const auto length = r.Read<std::uint16_t>();
                if (length == 0 || length > 4096) throw std::runtime_error("material name");
                r.Skip(length);
            }
            // Metadata is immutable in the replacement; retain native indices.
            r.Skip(named ? 24 : 20);
        }
        const std::uint64_t block = static_cast<std::uint64_t>(layout.data_offset) + relative - 8;
        const std::uint64_t end = block + 8 + indices[i] * 2ull + n * (kind == 1 ? 92ull : 76ull);
        if (block != previous_end || end > bytes.size())
            throw std::runtime_error("data ranges");
        Reader data{bytes, static_cast<std::size_t>(block)};
        if (data.Read<std::uint32_t>() != indices[i] * 2 || data.Read<std::uint32_t>() != 2)
            throw std::runtime_error("index format");
        for (std::uint32_t j = 0; j < indices[i]; ++j)
            if (data.Read<std::uint16_t>() >= n) throw std::runtime_error("index range");
        const std::uint32_t stride = kind == 1 ? 52 : 12;
        if (kind == 1) data.Skip(n * 20ull);
        layout.vertex_blocks.push_back({static_cast<std::uint32_t>(data.cursor), n * stride,
                                       n, stride, kind == 1 ? 4u : 0u});
        previous_end = end;
    }
    if (r.cursor != layout.data_offset || previous_end != bytes.size())
        throw std::runtime_error("unaccounted bytes");
    return layout;
}
}
ModelValidationResult ValidateModelEdit(
    const std::span<const std::byte> original,
    const std::span<const std::byte> replacement,
    const ModelEditContract contract) {
    ModelValidationResult result;
    if (original.size() != replacement.size()) {
        result.error = L"model size changed; only same-topology edits are supported";
        return result;
    }
    try {
        result.layout = Parse(original);
        std::size_t unchanged_begin = 0;
        const auto unchanged = [&](const std::size_t end) {
            if (!std::equal(original.begin() + unchanged_begin, original.begin() + end,
                            replacement.begin() + unchanged_begin))
                throw std::runtime_error("immutable model bytes changed (header/bounds/topology/UV/skinning)");
            unchanged_begin = end;
        };
        for (const auto& block : result.layout.vertex_blocks) {
            const bool shape = contract == ModelEditContract::bounded_shape;
            // No normal/tangent layout has been verified for static geometry.
            if (shape && block.stride != 52) {
                unchanged(block.offset + block.size);
                continue;
            }
            const bool block_changed = !std::equal(original.begin() + block.offset,
                original.begin() + block.offset + block.size, replacement.begin() + block.offset);
            std::array<float, 3> low, high;
            low.fill((std::numeric_limits<float>::max)());
            high.fill((std::numeric_limits<float>::lowest)());
            for (std::uint32_t v = 0; v < block.vertex_count; ++v) {
                const auto pos = block.offset + v * block.stride + block.position_offset;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float value = FloatAt(original, pos + axis * 4);
                    if (!std::isfinite(value)) throw std::runtime_error("original position");
                    low[axis] = (std::min)(low[axis], value);
                    high[axis] = (std::max)(high[axis], value);
                }
            }
            for (std::uint32_t v = 0; v < block.vertex_count; ++v) {
                const auto pos = block.offset + v * block.stride + block.position_offset;
                unchanged(pos);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float value = FloatAt(replacement, pos + axis * 4);
                    const double tolerance = (std::max)(1.0, static_cast<double>(high[axis]) - low[axis]) * 0.00001;
                    if (!std::isfinite(value) || value < low[axis] - tolerance ||
                        value > high[axis] + tolerance)
                        throw std::runtime_error("position exceeds original local bounds");
                }
                unchanged_begin = pos + 12;
                if (shape && block_changed) {
                    const auto vertex = block.offset + v * block.stride;
                    unchanged(vertex + 20); // +16 is unknown and must be preserved.
                    ValidateBasis(original, vertex);
                    ValidateBasis(replacement, vertex);
                    unchanged_begin = vertex + 52;
                }
            }
        }
        unchanged(original.size());
    } catch (const std::exception& error) {
        result.layout = {};
        const std::string reason = error.what();
        result.error.assign(reason.begin(), reason.end());
    }
    return result;
}
ModelValidationResult ValidateModelPositionEdit(
    const std::span<const std::byte> original,
    const std::span<const std::byte> replacement) {
    return ValidateModelEdit(original, replacement, ModelEditContract::positions_only);
}
}
