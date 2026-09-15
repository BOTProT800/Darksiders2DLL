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
    result.error = L"invalid INI: expected [loader], enabled=true|false, mode=override|observe";
    if (text.empty() || text.size() > 16 * 1024) return result;
    for (const unsigned char c : text) {
        if ((c < 32 && c != '\t' && c != '\r' && c != '\n') || c > 126) return result;
    }
    bool section = false, enabled = false, mode = false;
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
