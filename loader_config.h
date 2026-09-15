#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace ds2::modding {
struct LoaderConfig final {
    bool enabled{true};
    bool write_enabled{true};
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
