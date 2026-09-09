#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace ds2::modding {

using Sha256Digest = std::array<std::uint8_t, 32>;

enum class Sha256Error {
    none,
    invalid_argument,
    open_failed,
    metadata_failed,
    file_too_large,
    read_failed,
    crypto_failed,
};

struct Sha256Result final {
    Sha256Digest digest{};
    std::uint64_t file_size{};
    Sha256Error error{Sha256Error::none};
    unsigned long win32_error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == Sha256Error::none;
    }
};

[[nodiscard]] Sha256Result ComputeSha256(std::span<const std::byte> bytes);

// max_file_size is checked before reading. UINT64_MAX means no policy limit.
[[nodiscard]] Sha256Result ComputeFileSha256(
    const std::filesystem::path& path,
    std::uint64_t max_file_size = UINT64_MAX);

[[nodiscard]] std::string Sha256Hex(const Sha256Digest& digest);
[[nodiscard]] std::wstring Sha256HexWide(const Sha256Digest& digest);
[[nodiscard]] bool Sha256EqualsHex(
    const Sha256Digest& digest,
    std::wstring_view expected_hex) noexcept;

}  // namespace ds2::modding
