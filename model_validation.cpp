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
    // Offsets grow with the file (over 1 MB in large models), unlike counts.
    std::uint32_t Offset() {
        const std::int64_t value = Read<std::int32_t>();
        if (value < 0 || static_cast<std::uint64_t>(value) > bytes.size()) throw std::runtime_error("offset");
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
std::uint16_t U16At(const std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint16_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
// Header positions of every field a deletion recomputes (Anansi prune_dcm).
struct MeshFields final {
    std::uint8_t kind{};
    std::uint32_t vertices{}, indices{};
    std::size_t index_entry{}, vertex_entry{}, count_field{}, relative_field{};
    std::vector<std::size_t> spans; // named material: i32 vertex count/start, index count/start
    bool unnamed_material{};
    std::uint64_t block{}, end{};
    // Every material: i32 vertex count/start, index count (+ index start if named).
    struct Material final { std::size_t counts{}; bool named{}; };
    std::vector<Material> materials;
};
struct ParsedModel final {
    ModelLayout layout;
    std::vector<MeshFields> meshes;
    std::size_t pairs_offset{}, pairs_size{}; // {i32, f32} per mesh; statics keep a vertex count
};
ParsedModel Parse(const std::span<const std::byte> bytes) {
    if (bytes.size() < 32 || bytes.size() > 64ull * 1024 * 1024)
        throw std::runtime_error("size");
    Reader r{bytes};
    ParsedModel parsed;
    auto& layout = parsed.layout;
    layout.data_offset = r.Read<std::uint32_t>();
    const auto meshes = r.Count();
    if (meshes == 0 || meshes > 256 || layout.data_offset > bytes.size())
        throw std::runtime_error("header");
    if (r.Count() != meshes) throw std::runtime_error("index table");
    parsed.meshes.resize(meshes);
    std::vector<std::uint32_t> indices(meshes), vertices(meshes);
    for (std::uint32_t i = 0; i < meshes; ++i) {
        parsed.meshes[i].index_entry = r.cursor;
        indices[i] = r.Count();
    }
    if (r.Count(true) != meshes) throw std::runtime_error("vertex table");
    for (std::uint32_t i = 0; i < meshes; ++i) {
        parsed.meshes[i].vertex_entry = r.cursor;
        vertices[i] = r.Count(true);
    }
    const auto extra = r.Count(true);
    parsed.pairs_offset = r.cursor;
    parsed.pairs_size = static_cast<std::size_t>(extra) * 8;
    r.Skip(parsed.pairs_size);
    layout.reads.push_back({0, layout.data_offset});
    std::uint64_t previous_end = layout.data_offset;
    for (std::uint32_t i = 0; i < meshes; ++i) {
        auto& fields = parsed.meshes[i];
        const auto kind = r.Read<std::uint8_t>();
        fields.count_field = r.cursor;
        const auto n = r.Count();
        const auto header_stride = r.Count();
        fields.relative_field = r.cursor;
        const auto relative = r.Offset();
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
                fields.spans.push_back(r.cursor + 8);
            } else {
                fields.unnamed_material = true;
            }
            fields.materials.push_back({r.cursor + 8, named});
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
                                       n, stride, kind == 1 ? 4u : 0u,
                                       static_cast<std::uint32_t>(block + 8), indices[i]});
        auto at = static_cast<std::uint32_t>(block + 8);
        layout.reads.push_back({at, indices[i] * 2});
        at += indices[i] * 2;
        for (const std::uint32_t record : kind == 1 ? std::initializer_list<std::uint32_t>{20, 52, 16, 4}
                                                    : std::initializer_list<std::uint32_t>{12, 64}) {
            layout.reads.push_back({at, n * record});
            at += n * record;
        }
        fields.kind = kind;
        fields.vertices = n;
        fields.indices = indices[i];
        fields.block = block;
        fields.end = end;
        previous_end = end;
    }
    if (r.cursor != layout.data_offset || previous_end != bytes.size())
        throw std::runtime_error("unaccounted bytes");
    return parsed;
}
// Skinned block: u32 index bytes, u32 2, u16 indices, then per vertex records
// of 20 (attributes), 52 (vertex), 16 (weights) and 4 (bone indices) bytes.
struct SkinnedRecords final {
    std::size_t indices{}, attributes{}, vertices{}, weights{}, joints{};
};
SkinnedRecords Records(const MeshFields& mesh) {
    SkinnedRecords at;
    at.indices = static_cast<std::size_t>(mesh.block) + 8;
    at.attributes = at.indices + mesh.indices * 2ull;
    at.vertices = at.attributes + mesh.vertices * 20ull;
    at.weights = at.vertices + mesh.vertices * 52ull;
    at.joints = at.weights + mesh.vertices * 16ull;
    return at;
}
// Fields a shape edit may not touch: everything but position/normal/tangent.
bool SameImmutableRecord(const std::span<const std::byte> a, const SkinnedRecords& at_a, const std::size_t va,
                         const std::span<const std::byte> b, const SkinnedRecords& at_b, const std::size_t vb) {
    const auto same = [&](const std::size_t from_a, const std::size_t from_b, const std::size_t count) {
        return std::equal(a.begin() + from_a, a.begin() + from_a + count, b.begin() + from_b);
    };
    return same(at_a.attributes + va * 20, at_b.attributes + vb * 20, 20) &&
           same(at_a.vertices + va * 52, at_b.vertices + vb * 52, 4) &&
           same(at_a.vertices + va * 52 + 16, at_b.vertices + vb * 52 + 16, 4) &&
           same(at_a.weights + va * 16, at_b.weights + vb * 16, 16) &&
           same(at_a.joints + va * 4, at_b.joints + vb * 4, 4);
}
std::int64_t I32At(const std::span<const std::byte> bytes, const std::size_t offset) {
    std::int32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
struct MaterialSpan final {
    std::int64_t vertex_start{}, vertex_count{}, index_start{}, index_count{};
};
// Unnamed materials store no index start; theirs follow the previous span.
std::vector<MaterialSpan> Spans(const std::span<const std::byte> bytes, const MeshFields& mesh) {
    if (mesh.materials.empty()) return {{0, mesh.vertices, 0, mesh.indices}};
    std::vector<MaterialSpan> spans;
    std::int64_t next = 0;
    for (const auto& material : mesh.materials) {
        MaterialSpan span{I32At(bytes, material.counts + 4), I32At(bytes, material.counts),
                          material.named ? I32At(bytes, material.counts + 12) : next,
                          I32At(bytes, material.counts + 8)};
        if (span.vertex_start < 0 || span.vertex_count < 0 || span.index_count < 0 ||
            span.index_start < next || span.index_count % 3 != 0 || span.index_start % 3 != 0 ||
            span.vertex_start + span.vertex_count > mesh.vertices ||
            span.index_start + span.index_count > mesh.indices)
            throw std::runtime_error("material span out of range or overlapping");
        next = span.index_start + span.index_count;
        spans.push_back(span);
    }
    return spans;
}
// Per-vertex arrays after the index buffer, as {offset, record size}.
std::vector<std::pair<std::size_t, std::size_t>> VertexArrays(const MeshFields& mesh) {
    std::vector<std::pair<std::size_t, std::size_t>> arrays;
    auto at = static_cast<std::size_t>(mesh.block) + 8 + mesh.indices * 2ull;
    for (const std::size_t record : mesh.kind == 1 ? std::initializer_list<std::size_t>{20, 52, 16, 4}
                                                   : std::initializer_list<std::size_t>{12, 64}) {
        arrays.emplace_back(at, record);
        at += mesh.vertices * record;
    }
    return arrays;
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
        result.layout = Parse(original).layout;
        if (contract == ModelEditContract::unchecked) {
            // No content rule: the engine only needs every read at its original size.
            if (Parse(replacement).layout.reads != result.layout.reads)
                throw std::runtime_error("replacement read sizes differ from the original");
            return result;
        }
        std::size_t unchanged_begin = 0;
        const auto unchanged = [&](const std::size_t end) {
            if (!std::equal(original.begin() + unchanged_begin, original.begin() + end,
                            replacement.begin() + unchanged_begin))
                throw std::runtime_error("immutable model bytes changed (header/bounds/topology/UV/skinning)");
            unchanged_begin = end;
        };
        for (const auto& block : result.layout.vertex_blocks) {
            const bool shape = contract == ModelEditContract::bounded_shape;
            // A removed triangle keeps its slot as (a,a,a): zero area, same
            // buffer size, and a stays inside the original material span.
            unchanged(block.index_offset);
            for (std::size_t at = block.index_offset; at < block.index_offset + block.index_count * 2ull; at += 6) {
                if (std::equal(original.begin() + at, original.begin() + at + 6, replacement.begin() + at))
                    continue;
                if (!shape)
                    throw std::runtime_error("immutable model bytes changed (header/bounds/topology/UV/skinning)");
                const auto a = U16At(replacement, at);
                if (a != U16At(replacement, at + 2) || a != U16At(replacement, at + 4) ||
                    (a != U16At(original, at) && a != U16At(original, at + 2) && a != U16At(original, at + 4)))
                    throw std::runtime_error("index buffer changed beyond collapsing removed triangles");
            }
            unchanged_begin = block.index_offset + block.index_count * 2ull;
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
ModelExpansionResult ExpandModelDeletion(const std::span<const std::byte> original,
                                         const std::span<const std::byte> pruned) {
    ModelExpansionResult result;
    try {
        if (pruned.size() >= original.size())
            throw std::runtime_error("a deletion must make the model smaller");
        const auto before = Parse(original);
        const auto after = Parse(pruned);
        const auto header_size = before.layout.data_offset;
        if (after.layout.data_offset != header_size || after.meshes.size() != before.meshes.size())
            throw std::runtime_error("header layout changed");
        // The pruned header is the original with only the deletion counts
        // rewritten; bounds, bones, materials and unknown fields stay.
        std::vector<std::byte> header(original.begin(), original.begin() + header_size);
        for (std::size_t i = 0; i < before.meshes.size(); ++i) {
            const auto& o = before.meshes[i];
            const auto& e = after.meshes[i];
            if (e.index_entry != o.index_entry || e.vertex_entry != o.vertex_entry ||
                e.count_field != o.count_field || e.relative_field != o.relative_field || e.spans != o.spans)
                throw std::runtime_error("header layout changed");
            for (const auto field : {o.index_entry, o.vertex_entry, o.count_field, o.relative_field})
                std::copy_n(pruned.begin() + field, 4, header.begin() + field);
            for (const auto span : o.spans)
                std::copy_n(pruned.begin() + span, 16, header.begin() + span);
        }
        if (!std::equal(header.begin(), header.end(), pruned.begin()))
            throw std::runtime_error("header changed beyond deletion counts (bounds/bones/materials)");

        std::vector<std::byte> out(original.begin(), original.end());
        const auto corners = [](const std::span<const std::byte> bytes, const std::size_t at) {
            return std::array<std::uint16_t, 3>{U16At(bytes, at), U16At(bytes, at + 2), U16At(bytes, at + 4)};
        };
        for (std::size_t i = 0; i < before.meshes.size(); ++i) {
            const auto& o = before.meshes[i];
            const auto& e = after.meshes[i];
            if (e.vertices == o.vertices && e.indices == o.indices) {
                for (const auto span : o.spans)
                    if (!std::equal(original.begin() + span, original.begin() + span + 16, pruned.begin() + span))
                        throw std::runtime_error("material spans changed in a mesh without deletions");
                // Shape edits travel as is; bounded_shape validates them later.
                std::copy_n(pruned.begin() + e.block, o.end - o.block, out.begin() + o.block);
                continue;
            }
            if (o.kind != 1 || e.kind != 1)
                throw std::runtime_error("deletion is only supported in skinned meshes");
            if (o.unnamed_material)
                throw std::runtime_error("deletion in a mesh with unnamed materials is not verified");
            if (e.vertices > o.vertices || e.indices > o.indices)
                throw std::runtime_error("a pruned mesh can only lose vertices and triangles");
            const auto at_o = Records(o), at_e = Records(e);
            const std::size_t triangles_o = o.indices / 3, triangles_e = e.indices / 3;
            std::vector<std::int64_t> slot(e.vertices, -1), owner(o.vertices, -1);
            std::vector<bool> kept(triangles_o);
            const auto fits = [&](const std::array<std::uint16_t, 3>& ce, const std::array<std::uint16_t, 3>& co) {
                for (std::size_t k = 0; k < 3; ++k) {
                    const auto v = ce[k], w = co[k];
                    if (slot[v] >= 0 ? slot[v] != w
                                     : owner[w] >= 0 || !SameImmutableRecord(pruned, at_e, v, original, at_o, w))
                        return false;
                    for (std::size_t j = 0; j < k; ++j)
                        if ((ce[j] == v) != (co[j] == w)) return false;
                }
                return true;
            };
            // Prune keeps the surviving triangles in their original order.
            std::size_t cursor = 0;
            for (std::size_t t = 0; t < triangles_e; ++t) {
                const auto ce = corners(pruned, at_e.indices + t * 6);
                while (cursor < triangles_o && !fits(ce, corners(original, at_o.indices + cursor * 6))) ++cursor;
                if (cursor == triangles_o)
                    throw std::runtime_error("pruned triangles are not a subsequence of the original (added, merged or reordered)");
                const auto co = corners(original, at_o.indices + cursor * 6);
                for (std::size_t k = 0; k < 3; ++k) {
                    slot[ce[k]] = co[k];
                    owner[co[k]] = ce[k];
                }
                kept[cursor++] = true;
            }
            for (std::uint32_t v = 0; v < e.vertices; ++v) {
                if (slot[v] < 0) throw std::runtime_error("pruned vertex without triangle");
                std::copy_n(pruned.begin() + at_e.vertices + v * 52ull, 52,
                            out.begin() + at_o.vertices + static_cast<std::size_t>(slot[v]) * 52);
            }
            for (std::size_t t = 0; t < triangles_o; ++t) {
                if (kept[t]) continue;
                const auto first = U16At(original, at_o.indices + t * 6);
                for (std::size_t k = 0; k < 3; ++k)
                    std::memcpy(out.data() + at_o.indices + t * 6 + k * 2, &first, sizeof(first));
            }
            result.removed_vertices += o.vertices - e.vertices;
            result.removed_triangles += static_cast<std::uint32_t>(triangles_o - triangles_e);
        }
        result.bytes = std::move(out);
    } catch (const std::exception& error) {
        result.bytes.clear();
        const std::string reason = error.what();
        result.error.assign(reason.begin(), reason.end());
    }
    return result;
}
ModelFitResult FitModelReplacement(const std::span<const std::byte> original,
                                   const std::span<const std::byte> replacement) {
    ModelFitResult result;
    try {
        const auto before = Parse(original);
        const auto after = Parse(replacement);
        // The engine reads the header at its original size; the counts and
        // offsets in it then size every later read of the member.
        const auto header_size = before.layout.data_offset;
        if (after.layout.data_offset != header_size)
            throw std::runtime_error("header size changed (bones/materials); the engine reads the original header size");
        if (after.meshes.size() != before.meshes.size() || after.pairs_size != before.pairs_size)
            throw std::runtime_error("mesh count changed");
        std::vector<std::byte> out(original.begin(), original.end());
        std::copy_n(replacement.begin(), header_size, out.begin());
        std::copy_n(original.begin() + before.pairs_offset, before.pairs_size, out.begin() + after.pairs_offset);
        for (std::size_t i = 0; i < before.meshes.size(); ++i) {
            const auto& o = before.meshes[i];
            const auto& e = after.meshes[i];
            const auto mesh = "mesh " + std::to_string(i) + ": ";
            if (e.kind != o.kind) throw std::runtime_error(mesh + "kind changed (skinned/static)");
            if (e.materials.size() != o.materials.size())
                throw std::runtime_error(mesh + "material count changed");
            if (e.vertices > o.vertices || e.indices > o.indices)
                throw std::runtime_error(mesh + std::to_string(e.vertices) + " vertices/" +
                    std::to_string(e.indices / 3) + " triangles exceed the original " +
                    std::to_string(o.vertices) + "/" + std::to_string(o.indices / 3) +
                    "; a larger mesh needs other read sizes");
            for (const auto& [from, to] : std::initializer_list<std::pair<std::size_t, std::size_t>>{
                     {o.index_entry, e.index_entry}, {o.vertex_entry, e.vertex_entry},
                     {o.count_field, e.count_field}, {o.relative_field, e.relative_field}})
                std::copy_n(original.begin() + from, 4, out.begin() + to);
            for (std::size_t k = 0; k < o.materials.size(); ++k) {
                if (e.materials[k].named != o.materials[k].named)
                    throw std::runtime_error(mesh + "material record kind changed");
                std::copy_n(original.begin() + o.materials[k].counts, o.materials[k].named ? 16 : 12,
                            out.begin() + e.materials[k].counts);
            }
            const auto spans_o = Spans(original, o), spans_e = Spans(replacement, e);
            const auto indices_o = static_cast<std::size_t>(o.block) + 8;
            const auto indices_e = static_cast<std::size_t>(e.block) + 8;
            // Nothing original is drawn: every slot collapses, then the new triangles go in.
            for (std::size_t t = 0; t < o.indices; t += 3) {
                const auto a = U16At(original, indices_o + t * 2);
                for (std::size_t k = 0; k < 3; ++k) std::memcpy(out.data() + indices_o + (t + k) * 2, &a, sizeof(a));
            }
            std::vector<std::int64_t> slot(e.vertices, -1), owner(o.vertices, -1);
            std::int64_t drawn = 0;
            for (std::size_t k = 0; k < spans_o.size(); ++k) {
                const auto& so = spans_o[k];
                const auto& se = spans_e[k];
                if (se.vertex_count > so.vertex_count || se.index_count > so.index_count)
                    throw std::runtime_error(mesh + "material " + std::to_string(k) + " is larger than the original one");
                for (auto v = se.vertex_start; v < se.vertex_start + se.vertex_count; ++v) {
                    const auto target = so.vertex_start + (v - se.vertex_start);
                    auto& assigned = slot[static_cast<std::size_t>(v)];
                    auto& taken = owner[static_cast<std::size_t>(target)];
                    if (assigned < 0) {
                        if (taken >= 0) throw std::runtime_error(mesh + "materials overlap in the original vertex slots");
                        assigned = target;
                        taken = v;
                    } else if (assigned != target) {
                        throw std::runtime_error(mesh + "materials share vertices at different offsets");
                    }
                }
                for (std::int64_t j = 0; j < se.index_count; ++j) {
                    const auto v = U16At(replacement, indices_e + static_cast<std::size_t>(se.index_start + j) * 2);
                    if (v < se.vertex_start || v >= se.vertex_start + se.vertex_count)
                        throw std::runtime_error(mesh + "a triangle uses a vertex outside its material");
                    const auto target = static_cast<std::uint16_t>(slot[v]);
                    std::memcpy(out.data() + indices_o + static_cast<std::size_t>(so.index_start + j) * 2,
                                &target, sizeof(target));
                }
                drawn += se.index_count;
            }
            const auto arrays_o = VertexArrays(o), arrays_e = VertexArrays(e);
            std::uint32_t used = 0;
            for (std::uint32_t v = 0; v < e.vertices; ++v) {
                if (slot[v] < 0) continue; // outside every material: never drawn
                ++used;
                for (std::size_t r = 0; r < arrays_o.size(); ++r) {
                    const auto record = arrays_o[r].second;
                    std::copy_n(replacement.begin() + arrays_e[r].first + v * record, record,
                                out.begin() + arrays_o[r].first + static_cast<std::size_t>(slot[v]) * record);
                }
            }
            result.spare_vertices += o.vertices - used;
            result.collapsed_triangles += static_cast<std::uint32_t>((o.indices - drawn) / 3);
        }
        if (Parse(out).layout.reads != before.layout.reads)
            throw std::runtime_error("fitted model does not keep the original read sizes");
        result.bytes = std::move(out);
    } catch (const std::exception& error) {
        result.bytes.clear();
        const std::string reason = error.what();
        result.error.assign(reason.begin(), reason.end());
    }
    return result;
}
bool ReadModelEngineCounts(const std::span<const std::byte> model, ModelEngineCounts& counts) noexcept {
    counts = {};
    try {
        if (model.size() < 32 || model.size() > 64ull * 1024 * 1024) return false;
        Reader r{model};
        counts.header_size = r.Read<std::uint32_t>();
        const auto meshes = r.Count();
        if (meshes == 0 || meshes > 256 || counts.header_size > model.size() || r.Count() != meshes) return false;
        counts.meshes.resize(meshes);
        for (auto& mesh : counts.meshes) mesh.indices = r.Count();
        if (r.Count(true) != meshes) return false;
        for (auto& mesh : counts.meshes) {
            mesh.vertices = r.Read<std::int32_t>();
            if (mesh.vertices < -1'000'000 || mesh.vertices > 1'000'000 || mesh.vertices == 0) return false;
        }
        counts.pairs = r.Count(true);
        if (counts.pairs > meshes) return false;
        for (std::uint32_t i = 0; i < counts.pairs; ++i) {
            counts.meshes[i].pair = r.Read<std::int32_t>();
            r.Skip(4); // float factor, not part of the package metadata
        }
        return true;
    } catch (...) {
        counts = {};
        return false;
    }
}
std::vector<ModelEngineBuffer> ModelEngineBuffers(const ModelEngineCounts& counts) {
    // MeshResourceArray 0x10867AC: sizes are masked to 31 bits by the engine.
    std::vector<ModelEngineBuffer> buffers{{counts.header_size, false}};
    const auto add = [&](const std::uint64_t size, const bool skipped = false) {
        if (size > 0x7FFFFFFFull) throw std::runtime_error("engine buffer exceeds 31 bits");
        buffers.push_back({static_cast<std::uint32_t>(size), skipped});
    };
    for (const auto& mesh : counts.meshes) {
        if (mesh.indices > 0) {
            add(8, true); // u32 byte count + u32 stride: registered with a null destination
            add(mesh.indices * 2ull);
        }
        if (mesh.vertices >= 0) {
            const std::uint64_t n = static_cast<std::uint32_t>(mesh.vertices);
            add(n * 20); add(n * 52); add(n * 16); add(n * 4);
            continue;
        }
        const std::uint64_t n = static_cast<std::uint64_t>(-static_cast<std::int64_t>(mesh.vertices));
        if (mesh.pair > 0 || mesh.pair == -1) {
            add(n * 12); add(n * 48);
            if (mesh.pair > 0) add(static_cast<std::uint64_t>(mesh.pair) * 16);
            else add(n * 8);
        } else if (mesh.pair == -2) {
            add(n * 4);
        } else if (mesh.pair == -3) {
            add(n * 8);
        } else if (mesh.pair == -4) {
            add(n * 12);
        } else {
            add(n * 12); add(n * 48);
        }
    }
    return buffers;
}
std::uint64_t ModelEngineSize(const ModelEngineCounts& counts) noexcept {
    try {
        std::uint64_t total = 0;
        for (const auto& buffer : ModelEngineBuffers(counts)) total += buffer.size;
        return total;
    } catch (...) {
        return 0;
    }
}
std::vector<std::byte> EncodeModelEngineExtra(const ModelEngineCounts& counts) {
    std::vector<std::byte> extra;
    const auto put = [&](const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) extra.push_back(static_cast<std::byte>(value >> shift));
    };
    put(counts.header_size);
    put(static_cast<std::uint32_t>(counts.meshes.size()));
    put(counts.pairs);
    for (std::size_t i = 0; i < counts.meshes.size(); ++i) {
        put(counts.meshes[i].indices);
        put(static_cast<std::uint32_t>(counts.meshes[i].vertices));
        if (i < counts.pairs) put(static_cast<std::uint32_t>(counts.meshes[i].pair));
    }
    return extra;
}
namespace {
// Header records after the count tables, checked against the engine layout:
// mesh kind matches the vertex sign, each block starts where the previous
// engine buffers end, and the records end exactly at the header size.
void CheckResizedHeader(const std::span<const std::byte> bytes, const ModelEngineCounts& counts) {
    Reader r{bytes};
    r.Skip(8 + 4 + counts.meshes.size() * 4ull + 4 + counts.meshes.size() * 4ull + 4 + counts.pairs * 8ull);
    std::uint64_t block = counts.header_size;
    for (std::size_t i = 0; i < counts.meshes.size(); ++i) {
        const auto& mesh = counts.meshes[i];
        const auto kind = r.Read<std::uint8_t>();
        const auto n = r.Count();
        const auto header_stride = r.Count();
        const auto relative = r.Offset();
        const auto field3 = r.Count(), field4 = r.Count();
        const bool skinned = mesh.vertices >= 0;
        const auto vertices = static_cast<std::uint32_t>(skinned ? mesh.vertices : -static_cast<std::int64_t>(mesh.vertices));
        if (kind != (skinned ? 1 : 0) || n != vertices || header_stride != (skinned ? 64u : 60u) ||
            mesh.indices % 3 != 0 || static_cast<std::uint64_t>(counts.header_size) + relative - 8 != block)
            throw std::runtime_error("mesh record disagrees with its counts or engine offset");
        for (int k = 0; k < 6; ++k)
            if (!std::isfinite(r.Read<float>())) throw std::runtime_error("bounds");
        const auto bones = skinned ? field4 : 0;
        const auto materials = skinned ? field3 : field4;
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
            r.Skip(named ? 24 : 20);
        }
        ModelEngineCounts one{0, i < counts.pairs ? 1u : 0u, {mesh}};
        block += ModelEngineSize(one);
    }
    if (r.cursor != counts.header_size) throw std::runtime_error("header records do not end at the header size");
    // Index buffers: the prefix the engine skips, and indices inside the mesh.
    block = counts.header_size;
    for (std::size_t i = 0; i < counts.meshes.size(); ++i) {
        const auto& mesh = counts.meshes[i];
        if (mesh.indices > 0) {
            Reader data{bytes, static_cast<std::size_t>(block)};
            const auto vertices = static_cast<std::uint32_t>(mesh.vertices >= 0 ? mesh.vertices : -static_cast<std::int64_t>(mesh.vertices));
            if (data.Read<std::uint32_t>() != mesh.indices * 2 || data.Read<std::uint32_t>() != 2)
                throw std::runtime_error("index format");
            for (std::uint32_t j = 0; j < mesh.indices; ++j)
                if (data.Read<std::uint16_t>() >= vertices) throw std::runtime_error("index range");
        }
        ModelEngineCounts one{0, i < counts.pairs ? 1u : 0u, {mesh}};
        block += ModelEngineSize(one);
    }
}
}
ModelResizePlan PlanModelResize(const std::span<const std::byte> original, const std::span<const std::byte> replacement,
                                const std::span<const std::byte> original_extra) {
    ModelResizePlan plan;
    try {
        ModelEngineCounts before, after;
        if (!ReadModelEngineCounts(original, before) || ModelEngineSize(before) != original.size())
            throw std::runtime_error("original layout is outside the engine model");
        const auto expected = EncodeModelEngineExtra(before);
        if (!std::equal(expected.begin(), expected.end(), original_extra.begin(), original_extra.end()))
            throw std::runtime_error("package metadata does not repeat the original header counts");
        if (!ReadModelEngineCounts(replacement, after))
            throw std::runtime_error("replacement count tables are invalid");
        if (after.meshes.size() != before.meshes.size() || after.pairs != before.pairs)
            throw std::runtime_error("mesh or pair count changed: the package metadata entry would change length");
        if (ModelEngineSize(after) != replacement.size() || replacement.size() > 0x7FFFFFFFull)
            throw std::runtime_error("replacement bytes differ from the engine buffers of its own counts");
        CheckResizedHeader(replacement, after);
        plan.extra = EncodeModelEngineExtra(after);
        plan.buffers = ModelEngineBuffers(after);
        plan.original_header_size = before.header_size;
        plan.replacement_header_size = after.header_size;
        if (plan.extra.size() != original_extra.size()) throw std::runtime_error("metadata length changed");
    } catch (const std::exception& error) {
        plan = {};
        const std::string reason = error.what();
        plan.error.assign(reason.begin(), reason.end());
    }
    return plan;
}
}
