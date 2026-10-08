#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace ds2::modding {
enum class ModelMode { off, observe, override_positions, override_shape };
// Observation only: every mode other than off hashes and matches the engine's
// reads of candidate members. No build writes animation bytes yet.
enum class AnimationMode { off, observe, override_keys };
struct LoaderConfig final {
    bool enabled{true};
    bool write_enabled{true};
    bool native_textures{true};
    ModelMode models{ModelMode::off};
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
