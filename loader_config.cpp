#include "pch.h"
#include "loader_config.h"
#include "byte_storage.h"

namespace ds2::modding {
namespace {
std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}
}
LoaderConfigResult ParseLoaderConfig(std::string_view text) {
    LoaderConfigResult result;
    result.error = L"invalid INI: expected [loader], enabled=true|false, mode=override|observe, textures=native|exact, models=off|observe|override_positions|override_shape, animations=off|observe|override_keys";
    if (text.empty() || text.size() > 16 * 1024) return result;
    for (const unsigned char c : text) {
        if ((c < 32 && c != '\t' && c != '\r' && c != '\n') || c > 126) return result;
    }
    bool section = false, enabled = false, mode = false, models = false, textures = false, animations = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = Trim(text.substr(0, end));
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line == "[loader]") {
            if (section) return result;
            section = true;
            continue;
        }
        if (!section) return result;
        const auto separator = line.find('=');
        if (separator == line.npos) return result;
        const auto key = Trim(line.substr(0, separator));
        const auto value = Trim(line.substr(separator + 1));
        if (key == "enabled" && !enabled) {
            if (value != "true" && value != "false") return result;
            result.config.enabled = value == "true";
            enabled = true;
        } else if (key == "mode" && !mode) {
            if (value != "override" && value != "observe") return result;
            result.config.write_enabled = value == "override";
            mode = true;
        } else if (key == "textures" && !textures) {
            if (value != "native" && value != "exact") return result;
            result.config.native_textures = value == "native";
            textures = true;
        } else if (key == "models" && !models) {
            if (value == "off") result.config.models = ModelMode::off;
            else if (value == "observe") result.config.models = ModelMode::observe;
            else if (value == "override_positions") result.config.models = ModelMode::override_positions;
            else if (value == "override_shape") result.config.models = ModelMode::override_shape;
            else return result;
            models = true;
        } else if (key == "animations" && !animations) {
            if (value == "off") result.config.animations = AnimationMode::off;
            else if (value == "observe") result.config.animations = AnimationMode::observe;
            else if (value == "override_keys") result.config.animations = AnimationMode::override_keys;
            else return result;
            animations = true;
        } else return result;
    }
    if (!section) return result;
    result.valid = true;
    result.error.clear();
    return result;
}
LoaderConfigResult LoadLoaderConfig(const std::filesystem::path& game_directory) {
    const auto path = game_directory / L"Darksiders2DLL.ini";
    const auto loaded = LoadByteStorageUnderRoot(game_directory, path, 16 * 1024);
    if (!loaded) {
        if (loaded.error == ByteLoadError::open_failed &&
            loaded.win32_error == ERROR_FILE_NOT_FOUND) return {{}, true, {}};
        return {{}, false, L"configuration unreadable, unsafe, empty or too large"};
    }
    const auto bytes = loaded.storage->Bytes();
    return ParseLoaderConfig({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}
}
