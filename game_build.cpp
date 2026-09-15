#include "game_build.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <limits>
#include <vector>

namespace ds2::modding {

GameBuildResult IdentifyGameBuild(const std::filesystem::path& executable_path) {
    GameBuildResult result;
    result.executable_path = executable_path;
    const auto hash = ComputeFileSha256(executable_path);
    result.digest = hash.digest;
    result.file_size = hash.file_size;
    result.sha256_error = hash.error;
    result.win32_error = hash.win32_error;
    if (!hash) {
        result.error = GameBuildError::hash_failed;
        return result;
    }
    result.sha256_hex = Sha256HexWide(hash.digest);
    if (!Sha256EqualsHex(hash.digest, kSupportedExecutableSha256)) {
        result.error = GameBuildError::unsupported_hash;
        return result;
    }
    result.build = GameBuildId::darksiders2_deathinitive_5580738;
    return result;
}

GameBuildResult IdentifyCurrentGameBuild() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            GameBuildResult result;
            result.error = GameBuildError::executable_path_failed;
            result.win32_error = GetLastError();
            return result;
        }
        if (length < buffer.size() - 1) {
            return IdentifyGameBuild(std::filesystem::path(
                std::wstring(buffer.data(), static_cast<std::size_t>(length))));
        }
        if (buffer.size() >= 32'768) {
            GameBuildResult result;
            result.error = GameBuildError::executable_path_failed;
            result.win32_error = ERROR_INSUFFICIENT_BUFFER;
            return result;
        }
        buffer.resize((std::min)(buffer.size() * 2, static_cast<std::size_t>(32'768)));
    }
}

std::wstring_view GameBuildIdName(const GameBuildId build) noexcept {
    switch (build) {
    case GameBuildId::unknown: return L"unknown";
    case GameBuildId::darksiders2_deathinitive_5580738:
        return L"Darksiders II Deathinitive 5580738";
    }
    return L"unknown";
}

std::wstring_view GameBuildErrorName(const GameBuildError error) noexcept {
    switch (error) {
    case GameBuildError::none: return L"none";
    case GameBuildError::executable_path_failed: return L"executable_path_failed";
    case GameBuildError::hash_failed: return L"hash_failed";
    case GameBuildError::unsupported_hash: return L"unsupported_hash";
    }
    return L"unknown";
}

}  // namespace ds2::modding
