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
std::vector<std::byte> MakeSkinnedModel(const std::vector<std::uint32_t>& ids, const Triangles& triangles) {
    const auto n = static_cast<std::uint32_t>(ids.size());
    const auto ic = static_cast<std::uint32_t>(triangles.size() * 3);
    const std::size_t data = 101, records = data + 8 + ic * 2ull;
    std::vector<std::byte> bytes(records + n * 92ull);
    Put(bytes, 0, static_cast<std::uint32_t>(data)); Put(bytes, 4, 1u); Put(bytes, 8, 1u);
    Put(bytes, 12, ic); Put(bytes, 16, 1u); Put(bytes, 20, n);
    bytes[28] = std::byte{1};
    Put(bytes, 29, n); Put(bytes, 33, 64u); Put(bytes, 37, 8u); Put(bytes, 41, 1u);
    for (std::size_t axis = 0; axis < 3; ++axis) Put(bytes, 61 + axis * 4, 4.0f);
    bytes[73] = std::byte{255}; Put(bytes, 74, std::uint16_t{1}); bytes[76] = std::byte{'M'};
    std::uint16_t low = 0xFFFF, high = 0;
    for (const auto& triangle : triangles)
        for (const auto v : triangle) { low = (std::min)(low, v); high = (std::max)(high, v); }
    Put(bytes, 85, static_cast<std::uint32_t>(high - low + 1)); Put(bytes, 89, std::uint32_t{low});
    Put(bytes, 93, ic);
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
        Require(candidate.ranges.size() == 4, "missing model read ranges");
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
    Require(ParseLoaderConfig("[loader]").config.models == ModelMode::off, "models default on");
    for (const auto* value : {"off", "observe", "override_positions", "override_shape"}) {
        const auto cfg = ParseLoaderConfig(std::string("[loader]\nmodels=") + value);
        Require(cfg.valid, "model mode rejected");
    }
    const auto observe = ParseLoaderConfig("[loader]\nmode=observe\nmodels=override_positions");
    Require(observe.valid && !observe.config.write_enabled, "global observation lost");
    const auto shape_observe = ParseLoaderConfig("[loader]\nmode=observe\nmodels=override_shape");
    Require(shape_observe.valid && !shape_observe.config.write_enabled &&
            shape_observe.config.models == ModelMode::override_shape, "shape global observation lost");
    for (const auto* value : {"models=override", "models=true", "models=off\nmodels=observe"})
        Require(!ParseLoaderConfig(std::string("[loader]\n") + value).valid, "invalid model config accepted");
    std::cout << "model_validation: PASS (legacy positions, bounded shape, orthonormal basis, immutable bounds/UV/skin, deletion expansion, malformed inputs, opt-in)\n";
}

int CheckModelFiles(const std::filesystem::path& original, const std::filesystem::path& modified,
                    const std::filesystem::path& expanded) {
    using namespace ds2::modding;
    const auto before = LoadByteStorage(original, 64ull * 1024 * 1024);
    auto after = LoadByteStorage(modified, 64ull * 1024 * 1024);
    if (!before || !after) {
        std::wcerr << L"MODEL_CHECK_FAILED: cannot read files: " << (!before ? before.detail : after.detail) << L'\n';
        return 1;
    }
    const auto modified_hash = after.storage->Sha256Hex();
    const auto modified_size = after.storage->Size();
    const bool deleted = modified_size < before.storage->Size();
    ModelExpansionResult deletion;
    if (deleted) {
        deletion = ExpandModelDeletion(before.storage->Bytes(), after.storage->Bytes());
        if (!deletion) { std::wcerr << L"MODEL_REJECTED: deletion: " << deletion.error << L'\n'; return 1; }
        after = MakeByteStorage(deletion.bytes);
        if (!after) { std::wcerr << L"MODEL_CHECK_FAILED: expanded storage\n"; return 1; }
        if (!expanded.empty()) Write(expanded, after.storage->Bytes());
    }
    const auto result = ValidateModelEdit(before.storage->Bytes(), after.storage->Bytes(), ModelEditContract::bounded_shape);
    if (!result) { std::wcerr << L"MODEL_REJECTED: " << result.error << L'\n'; return 1; }
    std::wcout << L"MODEL_CHECK_PASS contract=bounded_shape bytes=" << before.storage->Size()
               << L" meshes=" << result.layout.vertex_blocks.size()
               << L" changed=" << (before.storage->Sha256() != after.storage->Sha256());
    if (deleted)
        std::wcout << L" deletion_expanded=true mod_bytes=" << modified_size
                   << L" removed_vertices=" << deletion.removed_vertices
                   << L" removed_triangles=" << deletion.removed_triangles;
    std::wcout << L"\noriginal_sha256=" << before.storage->Sha256Hex()
               << L"\nmodified_sha256=" << modified_hash;
    if (deleted) std::wcout << L"\nexpanded_sha256=" << after.storage->Sha256Hex();
    std::wcout << L'\n';
    return 0;
}

int CheckModelCatalog(const std::filesystem::path& game, const std::filesystem::path& mods_path) {
    using namespace ds2::modding;
    ModIndexOptions options; options.dds_only = true; options.include_native_models = true;
    const auto mods = BuildModIndex(mods_path, options);
    if (!mods) { std::wcerr << L"MODEL_CATALOG_FAILED: mod index\n"; return 1; }
    const auto built = BuildModelCandidates(game, *mods.snapshot);
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
