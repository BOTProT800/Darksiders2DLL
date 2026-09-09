#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ds2::modding {

struct SignatureByte final {
    std::uint8_t value{};
    bool wildcard{};
};

using SignaturePattern = std::vector<SignatureByte>;

enum class SignatureParseError {
    none,
    empty,
    invalid_token,
};

struct SignatureParseResult final {
    SignaturePattern pattern;
    SignatureParseError error{SignatureParseError::none};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == SignatureParseError::none && !pattern.empty();
    }
};

enum class SignatureScanStatus {
    found,
    not_found,
    ambiguous,
    invalid_pattern,
    invalid_module,
    malformed_image,
};

struct SignatureScanResult final {
    SignatureScanStatus status{SignatureScanStatus::not_found};
    const std::byte* address{};
    std::size_t matches{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SignatureScanStatus::found && address != nullptr;
    }
};

// Parses signatures such as "48 8B ?? ?? 89". A wildcard must be exactly
// '?' or '??'; all concrete bytes must be two hexadecimal digits.
[[nodiscard]] SignatureParseResult ParseSignature(std::string_view text);

// Scans arbitrary bytes and succeeds only for exactly one match.
[[nodiscard]] SignatureScanResult ScanUnique(
    std::span<const std::byte> bytes,
    std::span<const SignatureByte> pattern) noexcept;

// Scans executable PE sections only. If section_name is non-empty, it must
// match the (at most eight-byte) PE section name exactly, for example ".text".
[[nodiscard]] SignatureScanResult ScanExecutableSectionsUnique(
    HMODULE module,
    std::span<const SignatureByte> pattern,
    std::string_view section_name = ".text") noexcept;

[[nodiscard]] std::string_view SignatureScanStatusName(
    SignatureScanStatus status) noexcept;

}  // namespace ds2::modding
