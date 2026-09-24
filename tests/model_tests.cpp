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
        Require(candidate.ranges.size() == 3, "missing model read ranges");
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
    std::cout << "model_validation: PASS (legacy positions, bounded shape, orthonormal basis, immutable bounds/UV/skin, malformed inputs, opt-in)\n";
}

int CheckModelFiles(const std::filesystem::path& original, const std::filesystem::path& modified) {
    using namespace ds2::modding;
    const auto before = LoadByteStorage(original, 64ull * 1024 * 1024);
    const auto after = LoadByteStorage(modified, 64ull * 1024 * 1024);
    if (!before || !after) {
        std::wcerr << L"MODEL_CHECK_FAILED: cannot read files: " << (!before ? before.detail : after.detail) << L'\n';
        return 1;
    }
    const auto result = ValidateModelEdit(before.storage->Bytes(), after.storage->Bytes(), ModelEditContract::bounded_shape);
    if (!result) { std::wcerr << L"MODEL_REJECTED: " << result.error << L'\n'; return 1; }
    std::wcout << L"MODEL_CHECK_PASS contract=bounded_shape bytes=" << before.storage->Size()
               << L" meshes=" << result.layout.vertex_blocks.size()
               << L" changed=" << (before.storage->Sha256() != after.storage->Sha256())
               << L"\noriginal_sha256=" << before.storage->Sha256Hex()
               << L"\nmodified_sha256=" << after.storage->Sha256Hex() << L'\n';
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
                   << L" bytes=" << entry.storage->Size() << L'\n';
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
