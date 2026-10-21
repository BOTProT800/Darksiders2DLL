#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace ds2::modding {
// override_positions and override_shape are accepted as aliases of
// override_unchecked: their content rules were never seen failing in game.
enum class ModelMode { off, observe, override_unchecked };
// Observation only: every mode other than off hashes and matches the engine's
// reads of candidate members. No build writes animation bytes yet.
enum class AnimationMode { off, observe, override_keys };
struct LoaderConfig final {
    bool enabled{true};
    bool write_enabled{true};
    bool native_textures{true};
    // textures=resize (the default, confirmed in game): native, plus delivery
    // at the texture's own size. textures=native resamples to the original.
    bool resize_textures{true};
    // override_unchecked by default: it and mesh resize were confirmed in
    // game, and nothing seen in game justifies keeping models off.
    ModelMode models{ModelMode::override_unchecked};
    AnimationMode animations{AnimationMode::off};
};
struct LoaderConfigResult final {
    LoaderConfig config;
    bool valid{};
    std::wstring error;
};
// Strict ASCII INI, at most 16 KiB. Missing file uses the documented defaults;
// all other read or syntax failures disable initialization.
[[nodiscard]] LoaderConfigResult ParseLoaderConfig(std::string_view text);
[[nodiscard]] LoaderConfigResult LoadLoaderConfig(const std::filesystem::path& game_directory);
}
