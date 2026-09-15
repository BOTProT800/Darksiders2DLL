#pragma once

#include "sha256.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ds2::modding {

inline constexpr std::wstring_view kSupportedExecutableSha256 =
    L"5580738EF70BC5BBCC72D7DC4A9C319956CD14DBFEF6F9DBEC54C1B5D97799FB";

enum class GameBuildId {
    unknown,
    darksiders2_deathinitive_5580738,
};

enum class GameBuildError {
    none,
    executable_path_failed,
    hash_failed,
    unsupported_hash,
};

struct GameBuildResult final {
    GameBuildId build{GameBuildId::unknown};
    GameBuildError error{GameBuildError::none};
    Sha256Error sha256_error{Sha256Error::none};
    unsigned long win32_error{};
    std::filesystem::path executable_path;
    Sha256Digest digest{};
    std::wstring sha256_hex;
    std::uint64_t file_size{};

    [[nodiscard]] bool Supported() const noexcept {
        return error == GameBuildError::none && build != GameBuildId::unknown;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return Supported(); }
};

// A build is supported only when the complete executable SHA-256 exactly
// matches the allow-list. Errors and unknown hashes are deliberately closed.
[[nodiscard]] GameBuildResult IdentifyGameBuild(
    const std::filesystem::path& executable_path);
[[nodiscard]] GameBuildResult IdentifyCurrentGameBuild();

[[nodiscard]] std::wstring_view GameBuildIdName(GameBuildId build) noexcept;
[[nodiscard]] std::wstring_view GameBuildErrorName(GameBuildError error) noexcept;

}  // namespace ds2::modding
