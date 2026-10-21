#include "model_tests.h"
#include "general_resolver_runtime_tests.h"
#include "loader_config.h"
#include "model_validation.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace ds2::modding;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> void Put(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "fixture offset");
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(out.good(), "model fixture write failed");
}
using Triangles = std::vector<std::array<std::uint16_t, 3>>;
// One skinned mesh with one named material. Every record derives from its
// source vertex id, so a pruned copy keeps the surviving records byte-identical,
// as Anansi prune_dcm does.
// A longer material name changes the header size (101 bytes with one letter).
std::vector<std::byte> MakeSkinnedModel(const std::vector<std::uint32_t>& ids, const Triangles& triangles,
                                        const std::string_view material = "M") {
    const auto n = static_cast<std::uint32_t>(ids.size());
    const auto ic = static_cast<std::uint32_t>(triangles.size() * 3);
    const std::size_t name = material.size(), fields = 76 + name;
    const std::size_t data = fields + 24, records = data + 8 + ic * 2ull;
    std::vector<std::byte> bytes(records + n * 92ull);
    Put(bytes, 0, static_cast<std::uint32_t>(data)); Put(bytes, 4, 1u); Put(bytes, 8, 1u);
    Put(bytes, 12, ic); Put(bytes, 16, 1u); Put(bytes, 20, n);
    bytes[28] = std::byte{1};
    Put(bytes, 29, n); Put(bytes, 33, 64u); Put(bytes, 37, 8u); Put(bytes, 41, 1u);
    for (std::size_t axis = 0; axis < 3; ++axis) Put(bytes, 61 + axis * 4, 4.0f);
    bytes[73] = std::byte{255}; Put(bytes, 74, static_cast<std::uint16_t>(name));
    for (std::size_t c = 0; c < name; ++c) bytes[76 + c] = static_cast<std::byte>(material[c]);
    std::uint16_t low = 0xFFFF, high = 0;
    for (const auto& triangle : triangles)
        for (const auto v : triangle) { low = (std::min)(low, v); high = (std::max)(high, v); }
    Put(bytes, fields + 8, static_cast<std::uint32_t>(high - low + 1)); Put(bytes, fields + 12, std::uint32_t{low});
    Put(bytes, fields + 16, ic);
    Put(bytes, data, ic * 2); Put(bytes, data + 4, 2u);
    for (std::size_t t = 0; t < triangles.size(); ++t)
        for (std::size_t k = 0; k < 3; ++k) Put(bytes, data + 8 + (t * 3 + k) * 2, triangles[t][k]);
    for (std::uint32_t v = 0; v < n; ++v) {
        Put(bytes, records + v * 20ull, static_cast<float>(ids[v])); // UV identifies the source vertex
        const auto vertex = records + n * 20ull + v * 52ull;
        Put(bytes, vertex, 1u);
        Put(bytes, vertex + 4, static_cast<float>(ids[v] % 3)); Put(bytes, vertex + 8, static_cast<float>(ids[v] / 3));
        Put(bytes, vertex + 28, 1.0f); Put(bytes, vertex + 32, 1.0f);
        Put(bytes, vertex + 36, 1.0f); Put(bytes, vertex + 48, 1.0f);
        Put(bytes, records + n * 72ull + v * 16ull, 1.0f);
    }
    return bytes;
}
void TestModelDeletion(const std::filesystem::path& temporary_root) {
    // Triangle n starts at 109 + 6n; vertex records start at 127 in the original.
    const auto original = MakeSkinnedModel({0, 1, 2, 3, 4}, {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}});
    const auto contract = ModelEditContract::bounded_shape;
    // Deleting vertex 1 removes two triangles and leaves vertex 0 without any.
    const auto pruned = MakeSkinnedModel({2, 3, 4}, {{0, 1, 2}});
    const auto expanded = ExpandModelDeletion(original, pruned);
    Require(bool(expanded) && expanded.removed_vertices == 2 && expanded.removed_triangles == 2,
            "pure deletion not expanded");
    auto collapsed = original;
    for (std::size_t k = 0; k < 3; ++k) {
        Put(collapsed, 109 + k * 2, std::uint16_t{0});
        Put(collapsed, 115 + k * 2, std::uint16_t{1});
    }
    Require(expanded.bytes == collapsed, "deletion must only collapse removed triangles in place");
    Require(bool(ValidateModelEdit(original, expanded.bytes, contract)), "expanded deletion rejected");
    Require(!ValidateModelEdit(original, expanded.bytes, ModelEditContract::positions_only),
            "positions contract accepted index edits");
    auto wrong = collapsed; Put(wrong, 113, std::uint16_t{1});
    Require(!ValidateModelEdit(original, wrong, contract), "partially collapsed triangle accepted");
    wrong = collapsed; for (std::size_t k = 0; k < 3; ++k) Put(wrong, 115 + k * 2, std::uint16_t{4});
    Require(!ValidateModelEdit(original, wrong, contract), "collapse outside the original triangle accepted");

    // Surviving vertices may also move; the record goes to its original slot.
    auto moved = MakeSkinnedModel({1, 2, 3, 4}, {{0, 2, 1}, {1, 2, 3}});
    Put(moved, 121 + 4 * 20 + 2 * 52 + 4, 0.5f); // records start at 121; pruned vertex 2 is original 3
    const auto shaped = ExpandModelDeletion(original, moved);
    float x{};
    if (shaped) std::memcpy(&x, shaped.bytes.data() + 127 + 5 * 20 + 3 * 52 + 4, sizeof(x));
    Require(bool(shaped) && shaped.removed_vertices == 1 && shaped.removed_triangles == 1 && x == 0.5f &&
            bool(ValidateModelEdit(original, shaped.bytes, contract)), "deletion with a shape edit rejected");

    for (int variant = 0; variant < 5; ++variant) {
        auto invalid = MakeSkinnedModel({1, 2, 3, 4}, {{0, 2, 1}, {1, 2, 3}});
        if (variant == 0) invalid = MakeSkinnedModel({1, 2, 3, 4}, {{1, 2, 3}, {0, 2, 1}}); // reordered
        if (variant == 1) Put(invalid, 121 + 4, 9.0f);   // UV of a surviving vertex
        if (variant == 2) Put(invalid, 49, -1.0f);       // header bounds
        if (variant == 3) Put(invalid, 121 + 4 * 72, 0.5f); // skin weight
        if (variant == 4) invalid = original;            // not smaller
        Require(!ExpandModelDeletion(original, invalid), "unsafe deletion accepted");
    }

    const auto mods_path = temporary_root / L"deletion-models";
    Write(mods_path / L"cut/media/characters/fixture.2", pruned);
    ModIndexOptions options; options.dds_only = true; options.include_native_models = true;
    const auto mods = BuildModIndex(mods_path, options);
    Require(mods && mods.snapshot->Assets().size() == 1, "deletion fixture not indexed");
    PackageIdentityEntry entry;
    entry.identity = {0x123400, 0x40, 223};
    entry.virtual_path = mods.snapshot->Assets().front().virtual_path;
    entry.type_id = 2; entry.original_size = static_cast<std::uint32_t>(original.size());
    ModelCandidate candidate; std::wstring error;
    Require(!PrepareModelCandidate(entry, mods.snapshot->Assets().front(), original, candidate, error,
                                   ModelEditContract::positions_only), "positions contract accepted a deletion");
    Require(PrepareModelCandidate(entry, mods.snapshot->Assets().front(), original, candidate, error, contract) &&
            candidate.storage->Size() == original.size() && candidate.mod_bytes == pruned.size() &&
            candidate.removed_vertices == 2 && candidate.removed_triangles == 2, "deletion candidate rejected");
    const auto index = std::find_if(candidate.ranges.begin(), candidate.ranges.end(),
        [](const ModelRange& range) { return range.offset == 109 && range.size == 18; });
    Require(index != candidate.ranges.end() && index->original_hash != index->replacement_hash &&
            MatchModelRange(candidate, index->size, index->original_hash) == &*index,
            "deletion index range missing");
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
    TestModelRuntime(candidate, original);
#endif
}
void TestModelResize(const std::filesystem::path& temporary_root) {
    // The engine model (mesh cache 0x75908C / 0x10867AC) on the synthetic layout.
    const auto original = MakeSkinnedModel({0, 1, 2, 3, 4}, {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}});
    ModelEngineCounts counts;
    Require(ReadModelEngineCounts(original, counts) && counts.header_size == 101 && counts.pairs == 0 &&
            counts.meshes.size() == 1 && counts.meshes[0].indices == 9 && counts.meshes[0].vertices == 5,
            "engine counts misread");
    const std::vector<ModelEngineBuffer> expected_buffers{{101, false}, {8, true}, {18, false},
                                                          {100, false}, {260, false}, {80, false}, {20, false}};
    Require(ModelEngineBuffers(counts) == expected_buffers && ModelEngineSize(counts) == original.size(),
            "engine buffer list differs from the skinned layout");
    const auto extra = EncodeModelEngineExtra(counts);
    std::vector<std::byte> expected_extra(20);
    Put(expected_extra, 0, 101u); Put(expected_extra, 4, 1u); Put(expected_extra, 8, 0u);
    Put(expected_extra, 12, 9u); Put(expected_extra, 16, 5u);
    Require(extra == expected_extra, "engine metadata encoding differs");
    // Static meshes (7 vertices, no indices): the pair integer picks the arrays.
    for (const auto& [pair, bytes] : std::array<std::pair<std::int32_t, std::uint64_t>, 6>{{
             {3, 468}, {-1, 476}, {-2, 28}, {-3, 56}, {-4, 84}, {0, 420}}}) {
        const ModelEngineCounts statics{0, 1, {{0, -7, pair}}};
        Require(ModelEngineSize(statics) == bytes, "static engine layout differs");
    }

    // 12 vertices and 8 triangles do not fit the 5/3 slot; the header grows too.
    const auto larger = MakeSkinnedModel({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
        {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {4, 5, 6}, {6, 7, 8}, {8, 9, 10}, {9, 10, 11}, {0, 5, 11}}, "Mask");
    Require(!FitModelReplacement(original, larger), "larger mesh fitted into the original slot");
    const auto plan = PlanModelResize(original, larger, extra);
    ModelEngineCounts larger_counts;
    Require(bool(plan) && ReadModelEngineCounts(larger, larger_counts) &&
            plan.extra == EncodeModelEngineExtra(larger_counts) && plan.buffers == ModelEngineBuffers(larger_counts) &&
            plan.original_header_size == 101 && plan.replacement_header_size == 104 && plan.extra.size() == extra.size(),
            "larger mesh resize rejected");
    // The package entry must repeat the original header and the replacement
    // must span exactly its engine buffers: without that the DLL cannot know
    // what to deliver. Indices outside the mesh or a record kind against the
    // vertex sign were never seen failing in game: delivered with a warning.
    Require(plan.warnings.empty(), "consistent mesh warned");
    auto wrong_extra = extra; wrong_extra[12] ^= std::byte{1};
    Require(!PlanModelResize(original, larger, wrong_extra), "metadata schema mismatch accepted");
    auto longer = larger; longer.push_back(std::byte{});
    Require(!PlanModelResize(original, longer, extra), "trailing replacement bytes accepted");
    auto bad_index = larger; Put(bad_index, 104 + 8, std::uint16_t{12});
    const auto index_plan = PlanModelResize(original, bad_index, extra);
    Require(bool(index_plan) && index_plan.warnings.size() == 1 &&
            index_plan.warnings[0].find(L"index range") != std::wstring::npos, "index outside the mesh refused");
    auto wrong_kind = larger; wrong_kind[28] = std::byte{0};
    const auto kind_plan = PlanModelResize(original, wrong_kind, extra);
    Require(bool(kind_plan) && kind_plan.warnings.size() == 1 &&
            kind_plan.warnings[0].find(L"untested: mesh record") == 0, "record kind against the vertex sign refused");

    // Package metadata block: unrelated entries, this member's, a final table.
    std::vector<std::byte> block(24);
    Put(block, 16, static_cast<std::uint32_t>(original.size()));
    Put(block, 20, static_cast<std::uint32_t>(extra.size()));
    block.insert(block.end(), extra.begin(), extra.end());
    block.resize(block.size() + 4);
    Put(block, block.size() - 4, static_cast<std::uint32_t>(original.size()));
    const auto mods_path = temporary_root / L"resize-models";
    Write(mods_path / L"big/media/characters/fixture.2", larger);
    ModIndexOptions options; options.dds_only = true; options.include_native_models = true;
    const auto mods = BuildModIndex(mods_path, options);
    Require(mods && mods.snapshot->Assets().size() == 1, "resize fixture not indexed");
    const auto& asset = mods.snapshot->Assets().front();
    PackageIdentityEntry entry;
    entry.identity = {0x123400, 0x40, 223};
    entry.virtual_path = asset.virtual_path;
    entry.type_id = 2; entry.original_size = static_cast<std::uint32_t>(original.size());
    entry.metadata_size = static_cast<std::uint32_t>(block.size());
    entry.metadata_sha256 = ComputeSha256(block).digest;
    entry.extra_offset = 24; entry.extra = extra;
    ModelCandidate candidate; std::wstring error;
    Require(PrepareModelCandidate(entry, asset, original, candidate, error, ModelEditContract::unchecked) &&
            candidate.resize && candidate.ranges.empty() && std::ranges::equal(candidate.storage->Bytes(), larger) &&
            candidate.resize->extra == plan.extra && candidate.resize->extra_offset == 24 &&
            candidate.resize->original_size == original.size() &&
            candidate.resize->metadata_sha256 == entry.metadata_sha256, "resize candidate rejected");
    auto no_metadata = entry; no_metadata.metadata_size = 0;
    ModelCandidate rejected;
    Require(!PrepareModelCandidate(no_metadata, asset, original, rejected, error, ModelEditContract::unchecked),
            "resize without a package metadata entry accepted");
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
    const std::vector<std::byte> next_member(64, std::byte{0xAB});
    TestModelResizeRuntime(candidate, original, block, next_member);
#endif
}
void TestModelUncheckedFit(const std::filesystem::path& temporary_root) {
    const auto original = MakeSkinnedModel({0, 1, 2, 3, 4}, {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}});
    const auto contract = ModelEditContract::unchecked;
    // A different mesh: new records, reversed winding and other bounds.
    auto replacement = MakeSkinnedModel({7, 8, 9}, {{2, 1, 0}});
    Put(replacement, 49, -1.0f);
    const auto fitted = FitModelReplacement(original, replacement);
    Require(bool(fitted) && fitted.spare_vertices == 2 && fitted.collapsed_triangles == 2, "new mesh not fitted");
    // Original counts, spans and index prefix; new header content; the new
    // triangle first and the rest collapsed over the original records.
    auto expected = original;
    Put(expected, 49, -1.0f);
    const std::array<std::uint16_t, 9> indices{2, 1, 0, 1, 1, 1, 2, 2, 2};
    for (std::size_t k = 0; k < indices.size(); ++k) Put(expected, 109 + k * 2, indices[k]);
    for (const auto& [from, to, record] : std::array<std::array<std::size_t, 3>, 4>{
             {{115, 127, 20}, {175, 227, 52}, {331, 487, 16}, {379, 567, 4}}})
        std::copy_n(replacement.begin() + from, 3 * record, expected.begin() + to);
    Require(fitted.bytes == expected, "fit must keep the original sizes and place the new mesh first");
    Require(bool(ValidateModelEdit(original, fitted.bytes, contract)), "unchecked contract rejected a fitted mesh");
    Require(!ValidateModelEdit(original, fitted.bytes, ModelEditContract::bounded_shape),
            "shape contract accepted new bounds");

    // Same counts: no content rule at all, the fit is the replacement itself.
    auto same = original;
    Put(same, 127, 5.0f);         // UV
    Put(same, 227 + 4, 99.0f);    // position far outside the original box
    Put(same, 487, 0.25f);        // skin weight
    const auto identity = FitModelReplacement(original, same);
    Require(bool(identity) && identity.bytes == same && identity.spare_vertices == 0 &&
            identity.collapsed_triangles == 0 && bool(ValidateModelEdit(original, same, contract)),
            "same-size replacement not accepted as is");

    // Physical limits only: the engine reads the original sizes.
    Require(!FitModelReplacement(original, MakeSkinnedModel({0, 1, 2, 3, 4, 5}, {{0, 1, 2}})),
            "larger vertex count accepted");
    Require(!FitModelReplacement(original, MakeSkinnedModel({0, 1, 2, 3, 4},
                                 {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {0, 2, 4}})), "more triangles accepted");
    Require(!FitModelReplacement(original, MakeTestModel(true)), "other header size accepted");

    const auto mods_path = temporary_root / L"unchecked-models";
    Write(mods_path / L"new/media/characters/fixture.2", replacement);
    ModIndexOptions options; options.dds_only = true; options.include_native_models = true;
    const auto mods = BuildModIndex(mods_path, options);
    Require(mods && mods.snapshot->Assets().size() == 1, "unchecked fixture not indexed");
    PackageIdentityEntry entry;
    entry.identity = {0x123400, 0x40, 223};
    entry.virtual_path = mods.snapshot->Assets().front().virtual_path;
    entry.type_id = 2; entry.original_size = static_cast<std::uint32_t>(original.size());
    ModelCandidate candidate; std::wstring error;
    Require(!PrepareModelCandidate(entry, mods.snapshot->Assets().front(), original, candidate, error,
                                   ModelEditContract::bounded_shape), "shape contract accepted a new mesh");
    Require(PrepareModelCandidate(entry, mods.snapshot->Assets().front(), original, candidate, error, contract) &&
            candidate.unchecked_fit && candidate.storage->Size() == original.size() &&
            std::ranges::equal(candidate.storage->Bytes(), expected) && candidate.spare_vertices == 2 &&
            candidate.collapsed_triangles == 2 && candidate.ranges.size() == 8, "unchecked candidate rejected");
    for (const auto& range : candidate.ranges)
        Require(MatchModelRange(candidate, range.size, range.original_hash) == &range, "unchecked read not identified");
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
    TestModelRuntime(candidate, original);
#endif
}
// A one-entry pair table after its count (offset 24): count -1 with {i32, f32},
// as in every packaged .2, or count 1 with a bare i32, which the extra
// generator 0x758A60 also reads but no packaged .2 uses.
std::vector<std::byte> WithPairTable(std::vector<std::byte> model, const std::int32_t count) {
    const std::size_t entry = count < 0 ? 8 : 4;
    std::uint32_t header{};
    std::memcpy(&header, model.data(), sizeof(header));
    model.insert(model.begin() + 28, entry, std::byte{});
    Put(model, 0, header + static_cast<std::uint32_t>(entry));
    Put(model, 24, count);
    Put(model, 28, std::int32_t{3}); // pair integer; a skinned mesh ignores it
    if (count < 0) Put(model, 32, 1.0f);
    return model;
}
void TestModelPairTable() {
    const auto mesh = MakeSkinnedModel({0, 1, 2, 3, 4}, {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}});
    const auto larger = MakeSkinnedModel({0, 1, 2, 3, 4, 5}, {{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {3, 4, 5}});
    const auto original = WithPairTable(mesh, -1);
    ModelEngineCounts counts;
    Require(ReadModelEngineCounts(original, counts) && counts.header_size == 109 && counts.pairs == 1 &&
            counts.meshes[0].pair == 3 && ModelEngineSize(counts) == original.size(), "negative pair table misread");
    const auto extra = EncodeModelEngineExtra(counts);
    const auto plan = PlanModelResize(original, WithPairTable(larger, -1), extra);
    Require(bool(plan) && plan.replacement_header_size == 109 && plan.extra.size() == extra.size() &&
            bool(ValidateModelEdit(original, original, ModelEditContract::unchecked)), "negative pair table rejected");
    // Positive: every reader refuses it and says why.
    const auto positive = WithPairTable(mesh, 1);
    const auto named = [](const std::wstring& error) { return error.find(L"positive pair count 1") != std::wstring::npos; };
    Require(!ReadModelEngineCounts(positive, counts) && counts.meshes.empty(), "positive pair count read");
    Require(named(PlanModelResize(original, WithPairTable(larger, 1), extra).error) &&
            named(FitModelReplacement(original, positive).error) &&
            named(ValidateModelEdit(positive, positive, ModelEditContract::unchecked).error),
            "positive pair count not refused by name");
}
}

// One triangle; both layouts exercise the real parser and runtime writer.
std::vector<std::byte> MakeTestModel(const bool skinned) {
    std::vector<std::byte> bytes(skinned ? 363 : 315);
    Put(bytes, 0, 73u); Put(bytes, 4, 1u); Put(bytes, 8, 1u);
    Put(bytes, 12, 3u); Put(bytes, 16, 1u); Put(bytes, 20, 3u);
    bytes[28] = skinned ? std::byte{1} : std::byte{};
    Put(bytes, 29, 3u); Put(bytes, 33, skinned ? 64u : 60u); Put(bytes, 37, 8u);
    Put(bytes, 61, 1.0f); Put(bytes, 65, 1.0f); Put(bytes, 69, 1.0f);
    Put(bytes, 73, 6u); Put(bytes, 77, 2u);
    Put(bytes, 81, std::uint16_t{0}); Put(bytes, 83, std::uint16_t{1}); Put(bytes, 85, std::uint16_t{2});
    const std::size_t base = skinned ? 151 : 87, stride = skinned ? 52 : 12;
    Put(bytes, base + stride, 1.0f); Put(bytes, base + stride + 8, 1.0f);
    Put(bytes, base + stride * 2 + 4, 1.0f);
    if (skinned) {
        for (std::size_t v = 0; v < 3; ++v) {
            const auto vertex = 147 + v * 52;
            Put(bytes, vertex + 28, 1.0f); Put(bytes, vertex + 32, 1.0f);
            Put(bytes, vertex + 36, 1.0f); Put(bytes, vertex + 48, 1.0f);
        }
    }
    return bytes;
}

void TestModels(const std::filesystem::path& temporary_root) {
    using namespace ds2::modding;
    for (const bool skinned : {false, true}) {
        const auto original = MakeTestModel(skinned);
        const auto layout = ValidateModelPositionEdit(original, original);
        Require(bool(layout) && layout.layout.vertex_blocks.size() == 1, "native model layout rejected");
        const auto block = layout.layout.vertex_blocks.front();
        const auto position = block.offset + block.stride + block.position_offset;
        auto edited = original;
        Put(edited, position, 0.5f);
        Require(bool(ValidateModelPositionEdit(original, edited)), "bounded position edit rejected");
        for (const std::size_t offset : {std::size_t{0}, std::size_t{81}, original.size() - 1}) {
            auto invalid = edited; invalid[offset] ^= std::byte{1};
            Require(!ValidateModelPositionEdit(original, invalid), "non-position edit accepted");
        }
        for (const float value : {2.0f, -1.0f, std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::quiet_NaN()}) {
            auto invalid = edited; Put(invalid, position, value);
            Require(!ValidateModelPositionEdit(original, invalid), "unsafe position accepted");
        }
        auto resized = edited; resized.push_back(std::byte{});
        Require(!ValidateModelPositionEdit(original, resized), "resized model accepted");
        for (std::size_t cut = 0; cut < original.size(); ++cut) {
            const auto truncated = std::span(original).first(cut);
            Require(!ValidateModelPositionEdit(truncated, truncated), "truncated original accepted");
        }
        for (const std::size_t offset : {std::size_t{4}, std::size_t{16}, std::size_t{20},
                                        std::size_t{24}, std::size_t{37}}) {
            auto invalid = original; Put(invalid, offset, 0x80000000u);
            Require(!ValidateModelPositionEdit(invalid, invalid), "invalid count/offset accepted");
        }
        if (skinned) {
            auto invalid = edited; invalid[block.offset] ^= std::byte{1};
            Require(!ValidateModelPositionEdit(original, invalid), "skin influence count changed");
            // Blender rewrites a full valid basis in a mesh after deformation.
            Put(edited, block.offset + 36, 0.0f);
            Put(edited, block.offset + 40, 1.0f);
            Put(edited, block.offset + 48, -1.0f);
            Require(!ValidateModelPositionEdit(original, edited), "legacy contract widened silently");
            Require(bool(ValidateModelEdit(original, edited, ModelEditContract::bounded_shape)),
                    "bounded shape with updated basis rejected");
            for (const auto offset : {block.offset, block.offset + 16, block.offset - 60,
                                      block.offset + block.size, block.offset + block.size + 48, 49u, 81u}) {
                invalid = edited; invalid[offset] ^= std::byte{1};
                Require(!ValidateModelEdit(original, invalid, ModelEditContract::bounded_shape),
                        "shape modified immutable attributes/indices/box/weights/bones");
            }
            for (const auto offset : {20u, 32u, 36u, 48u}) {
                for (const float value : {2.0f, std::numeric_limits<float>::infinity(),
                                         std::numeric_limits<float>::quiet_NaN()}) {
                    invalid = edited; Put(invalid, block.offset + offset, value);
                    Require(!ValidateModelEdit(original, invalid, ModelEditContract::bounded_shape),
                            "invalid normal/tangent value accepted");
                }
            }
            invalid = edited;
            Put(invalid, block.offset + 40, 0.0f); Put(invalid, block.offset + 44, 1.0f);
            Require(!ValidateModelEdit(original, invalid, ModelEditContract::bounded_shape),
                    "parallel normal/tangent accepted");
            auto unknown_layout = original;
            Put(unknown_layout, block.offset + 32, 0.0f);
            Require(!ValidateModelEdit(unknown_layout, edited, ModelEditContract::bounded_shape),
                    "unknown source normal layout accepted");
            invalid = edited; Put(invalid, position, 2.0f);
            Require(!ValidateModelEdit(original, invalid, ModelEditContract::bounded_shape),
                    "shape escaped original model-space bounds");
        } else {
            Require(!ValidateModelEdit(original, edited, ModelEditContract::bounded_shape),
                    "shape contract accepted static deformation");
        }
        const auto mods_path = temporary_root / (skinned ? L"skin-models" : L"static-models");
        Write(mods_path / L"vertex_demo/media/items/weapons/fixture.2", edited);
        const std::array<std::byte, 1> ignored{};
        for (const auto* name : {L"plugin.dll", L"fixture.o3d", L"fixture.fbx"})
            Write(mods_path / L"vertex_demo" / name, ignored);
        ModIndexOptions options; options.dds_only = true;
        const auto disabled = BuildModIndex(mods_path, options);
        Require(disabled && disabled.snapshot->Assets().empty(), "model indexing not opt-in");
        options.include_native_models = true;
        const auto mods = BuildModIndex(mods_path, options);
        Require(mods && mods.snapshot->Assets().size() == 1, "native model index filtering failed");
        const auto& asset = mods.snapshot->Assets().front();
        PackageIdentityEntry entry;
        entry.identity = {0x123400, 0x40, 223};
        entry.virtual_path = asset.virtual_path;
        entry.type_id = 2; entry.original_size = static_cast<std::uint32_t>(original.size());
        ModelCandidate candidate; std::wstring error;
        const auto contract = skinned ? ModelEditContract::bounded_shape : ModelEditContract::positions_only;
        Require(PrepareModelCandidate(entry, asset, original, candidate, error, contract), "model candidate rejected");
        // Full, payload, header, index buffer and every per-vertex array.
        Require(candidate.ranges.size() == (skinned ? 8u : 6u), "missing model read ranges");
        for (const auto& range : candidate.ranges)
            Require(MatchModelRange(candidate, range.size, range.original_hash) == &range,
                    "range identity/hash matching failed");
        auto ambiguous = candidate; ambiguous.ranges.push_back(candidate.ranges.back());
        Require(!MatchModelRange(ambiguous, candidate.ranges.back().size,
                                 candidate.ranges.back().original_hash), "ambiguous model range accepted");
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
        TestModelRuntime(candidate, original);
#endif
        entry.type_id = 6;
        Require(!PrepareModelCandidate(entry, asset, original, candidate, error), "wrong model type accepted");
    }
    TestModelDeletion(temporary_root);
    TestModelUncheckedFit(temporary_root);
    TestModelResize(temporary_root);
    TestModelPairTable();
    // override_unchecked by default; the old strict modes are its aliases.
    Require(ParseLoaderConfig("[loader]").config.models == ModelMode::override_unchecked, "models default not unchecked");
    Require(ParseLoaderConfig("[loader]\nmodels=off").config.models == ModelMode::off &&
            ParseLoaderConfig("[loader]\nmodels=observe").config.models == ModelMode::observe, "model mode lost");
    for (const auto* value : {"override_positions", "override_shape", "override_unchecked"}) {
        const auto cfg = ParseLoaderConfig(std::string("[loader]\nmodels=") + value);
        Require(cfg.valid && cfg.config.models == ModelMode::override_unchecked, "model mode is not unchecked");
    }
    const auto observe = ParseLoaderConfig("[loader]\nmode=observe\nmodels=override_positions");
    Require(observe.valid && !observe.config.write_enabled, "global observation lost");
    const auto shape_observe = ParseLoaderConfig("[loader]\nmode=observe\nmodels=override_shape");
    Require(shape_observe.valid && !shape_observe.config.write_enabled &&
            shape_observe.config.models == ModelMode::override_unchecked, "shape global observation lost");
    for (const auto* value : {"models=override", "models=true", "models=off\nmodels=observe"})
        Require(!ParseLoaderConfig(std::string("[loader]\n") + value).valid, "invalid model config accepted");
    std::cout << "model_validation: PASS (legacy positions, bounded shape, orthonormal basis, immutable bounds/UV/skin, deletion expansion, unchecked fit, malformed inputs, opt-in)\n";
}

int CheckModelFiles(const std::filesystem::path& original, const std::filesystem::path& modified,
                    const std::filesystem::path& expanded, const bool unchecked) {
    using namespace ds2::modding;
    const auto before = LoadByteStorage(original, 64ull * 1024 * 1024);
    auto after = LoadByteStorage(modified, 64ull * 1024 * 1024);
    if (!before || !after) {
        std::wcerr << L"MODEL_CHECK_FAILED: cannot read files: " << (!before ? before.detail : after.detail) << L'\n';
        return 1;
    }
    const auto modified_hash = after.storage->Sha256Hex();
    const auto modified_size = after.storage->Size();
    const bool deleted = !unchecked && modified_size < before.storage->Size();
    ModelExpansionResult deletion;
    ModelFitResult fit;
    if (unchecked) {
        fit = FitModelReplacement(before.storage->Bytes(), after.storage->Bytes());
        if (!fit) {
            // Larger than the slot: the package entry repeats the original
            // header counts (checked against the installed package by the catalog).
            ModelEngineCounts counts;
            const auto plan = ReadModelEngineCounts(before.storage->Bytes(), counts)
                ? PlanModelResize(before.storage->Bytes(), after.storage->Bytes(), EncodeModelEngineExtra(counts))
                : ModelResizePlan{{}, {}, 0, 0, {}, L"original count tables are invalid"};
            if (!plan) {
                std::wcerr << L"MODEL_REJECTED: unchecked: " << fit.error << L"; resize: " << plan.error << L'\n';
                return 1;
            }
            std::wcout << L"MODEL_CHECK_PASS contract=unchecked route=resize original_bytes=" << before.storage->Size()
                       << L" replacement_bytes=" << modified_size
                       << L" original_header=" << plan.original_header_size
                       << L" replacement_header=" << plan.replacement_header_size
                       << L" buffers=" << plan.buffers.size()
                       << L"\noriginal_sha256=" << before.storage->Sha256Hex()
                       << L"\nmodified_sha256=" << modified_hash << L'\n';
            return 0;
        }
        after = MakeByteStorage(fit.bytes);
        if (!after) { std::wcerr << L"MODEL_CHECK_FAILED: fitted storage\n"; return 1; }
        if (!expanded.empty()) Write(expanded, after.storage->Bytes());
    } else if (deleted) {
        deletion = ExpandModelDeletion(before.storage->Bytes(), after.storage->Bytes());
        if (!deletion) { std::wcerr << L"MODEL_REJECTED: deletion: " << deletion.error << L'\n'; return 1; }
        after = MakeByteStorage(deletion.bytes);
        if (!after) { std::wcerr << L"MODEL_CHECK_FAILED: expanded storage\n"; return 1; }
        if (!expanded.empty()) Write(expanded, after.storage->Bytes());
    }
    const auto contract = unchecked ? ModelEditContract::unchecked : ModelEditContract::bounded_shape;
    const auto result = ValidateModelEdit(before.storage->Bytes(), after.storage->Bytes(), contract);
    if (!result) { std::wcerr << L"MODEL_REJECTED: " << result.error << L'\n'; return 1; }
    std::wcout << L"MODEL_CHECK_PASS contract=" << (unchecked ? L"unchecked" : L"bounded_shape")
               << L" bytes=" << before.storage->Size()
               << L" meshes=" << result.layout.vertex_blocks.size()
               << L" changed=" << (before.storage->Sha256() != after.storage->Sha256());
    if (deleted)
        std::wcout << L" deletion_expanded=true mod_bytes=" << modified_size
                   << L" removed_vertices=" << deletion.removed_vertices
                   << L" removed_triangles=" << deletion.removed_triangles;
    if (unchecked)
        std::wcout << L" unchecked_fit=true mod_bytes=" << modified_size
                   << L" spare_vertices=" << fit.spare_vertices
                   << L" collapsed_triangles=" << fit.collapsed_triangles;
    std::wcout << L"\noriginal_sha256=" << before.storage->Sha256Hex()
               << L"\nmodified_sha256=" << modified_hash;
    if (deleted) std::wcout << L"\nexpanded_sha256=" << after.storage->Sha256Hex();
    if (unchecked) std::wcout << L"\nfitted_sha256=" << after.storage->Sha256Hex();
    std::wcout << L'\n';
    return 0;
}

int CheckModelCatalog(const std::filesystem::path& game, const std::filesystem::path& mods_path, const bool unchecked) {
    using namespace ds2::modding;
    ModIndexOptions options; options.dds_only = true; options.include_native_models = true;
    const auto mods = BuildModIndex(mods_path, options);
    if (!mods) { std::wcerr << L"MODEL_CATALOG_FAILED: mod index\n"; return 1; }
    const auto built = BuildModelCandidates(game, *mods.snapshot,
        unchecked ? ModelEditContract::unchecked : ModelEditContract::bounded_shape);
    for (const auto& issue : built.issues) std::wcerr << L"MODEL_REJECTED: " << issue << L'\n';
    if (!built.snapshot || !built.error.empty()) {
        std::wcerr << L"MODEL_CATALOG_FAILED: " << built.error << L'\n'; return 1;
    }
    for (const auto& entry : built.snapshot->entries) {
        std::wcout << L"MODEL_CANDIDATE path=" << entry.virtual_path.key
                   << L" package_base=" << entry.identity.package_base
                   << L" table=" << entry.identity.member_table_offset
                   << L" member=" << entry.identity.member_ordinal
                   << L" bytes=" << entry.storage->Size();
        if (entry.removed_vertices != 0 || entry.removed_triangles != 0)
            std::wcout << L" deletion_expanded=true mod_bytes=" << entry.mod_bytes
                       << L" removed_vertices=" << entry.removed_vertices
                       << L" removed_triangles=" << entry.removed_triangles;
        if (entry.resize)
            std::wcout << L" resize=true mod_bytes=" << entry.mod_bytes
                       << L" original_header=" << entry.resize->original_header_size
                       << L" replacement_header=" << entry.resize->replacement_header_size
                       << L" buffers=" << entry.resize->buffers.size()
                       << L" metadata_bytes=" << entry.resize->metadata_size
                       << L" extra_offset=" << entry.resize->extra_offset;
        else if (entry.unchecked_fit)
            std::wcout << L" unchecked_fit=true mod_bytes=" << entry.mod_bytes
                       << L" spare_vertices=" << entry.spare_vertices
                       << L" collapsed_triangles=" << entry.collapsed_triangles;
        std::wcout << L'\n';
        for (const auto& range : entry.ranges)
            std::wcout << L"  range offset=" << range.offset << L" size=" << range.size
                       << L" original_sha256=" << Sha256HexWide(range.original_hash)
                       << L" replacement_sha256=" << Sha256HexWide(range.replacement_hash) << L'\n';
    }
    const auto expected = std::count_if(mods.snapshot->Assets().begin(), mods.snapshot->Assets().end(),
        [](const auto& asset) { return IsNativeModelPath(asset.virtual_path.key); });
    if (!built.issues.empty() || expected == 0 || built.snapshot->entries.size() != static_cast<std::size_t>(expected))
        return 1;
    std::wcout << L"MODEL_CATALOG_PASS count=" << built.snapshot->entries.size() << L" (offline only)\n";
    return 0;
}
