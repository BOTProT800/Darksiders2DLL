#include "signature_scan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace ds2::modding {
namespace {

bool IsAsciiSpace(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool SectionNameEquals(
    const IMAGE_SECTION_HEADER& section,
    const std::string_view expected) noexcept {
    if (expected.empty()) {
        return true;
    }

    const auto* const first = reinterpret_cast<const char*>(section.Name);
    const auto* const nul = std::find(first, first + IMAGE_SIZEOF_SHORT_NAME, '\0');
    return expected == std::string_view(first, static_cast<std::size_t>(nul - first));
}

bool MatchesAt(
    const std::span<const std::byte> bytes,
    const std::span<const SignatureByte> pattern,
    const std::size_t offset) noexcept {
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (!pattern[index].wildcard &&
            std::to_integer<std::uint8_t>(bytes[offset + index]) != pattern[index].value) {
            return false;
        }
    }
    return true;
}

}  // namespace

SignatureParseResult ParseSignature(const std::string_view text) {
    SignatureParseResult result;
    std::size_t cursor = 0;

    while (cursor < text.size()) {
        while (cursor < text.size() && IsAsciiSpace(text[cursor])) {
            ++cursor;
        }
        if (cursor == text.size()) {
            break;
        }

        const std::size_t token_start = cursor;
        while (cursor < text.size() && !IsAsciiSpace(text[cursor])) {
            ++cursor;
        }
        const auto token = text.substr(token_start, cursor - token_start);
        if (token == "?" || token == "??") {
            result.pattern.push_back(SignatureByte{0, true});
            continue;
        }
        if (token.size() != 2) {
            result.pattern.clear();
            result.error = SignatureParseError::invalid_token;
            return result;
        }

        unsigned int parsed = 0;
        const auto conversion = std::from_chars(
            token.data(), token.data() + token.size(), parsed, 16);
        if (conversion.ec != std::errc{} || conversion.ptr != token.data() + token.size() ||
            parsed > (std::numeric_limits<std::uint8_t>::max)()) {
            result.pattern.clear();
            result.error = SignatureParseError::invalid_token;
            return result;
        }
        result.pattern.push_back(SignatureByte{static_cast<std::uint8_t>(parsed), false});
    }

    if (result.pattern.empty()) {
        result.error = SignatureParseError::empty;
    }
    return result;
}

SignatureScanResult ScanUnique(
    const std::span<const std::byte> bytes,
    const std::span<const SignatureByte> pattern) noexcept {
    if (pattern.empty()) {
        return {SignatureScanStatus::invalid_pattern, nullptr, 0};
    }
    if (bytes.size() < pattern.size()) {
        return {SignatureScanStatus::not_found, nullptr, 0};
    }

    const std::byte* match = nullptr;
    std::size_t matches = 0;
    const std::size_t last_start = bytes.size() - pattern.size();
    for (std::size_t offset = 0; offset <= last_start; ++offset) {
        if (!MatchesAt(bytes, pattern, offset)) {
            continue;
        }
        ++matches;
        if (matches == 1) {
            match = bytes.data() + offset;
        } else {
            return {SignatureScanStatus::ambiguous, nullptr, matches};
        }
    }

    if (matches == 0) {
        return {SignatureScanStatus::not_found, nullptr, 0};
    }
    return {SignatureScanStatus::found, match, 1};
}

SignatureScanResult ScanExecutableSectionsUnique(
    const HMODULE module,
    const std::span<const SignatureByte> pattern,
    const std::string_view section_name) noexcept {
    if (pattern.empty()) {
        return {SignatureScanStatus::invalid_pattern, nullptr, 0};
    }
    if (module == nullptr || section_name.size() > IMAGE_SIZEOF_SHORT_NAME) {
        return {SignatureScanStatus::invalid_module, nullptr, 0};
    }

    const auto* const image = reinterpret_cast<const std::byte*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }

    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.NumberOfSections == 0) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }

    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    const auto* const sections = IMAGE_FIRST_SECTION(nt);
    const std::byte* unique = nullptr;
    std::size_t total_matches = 0;

    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            !SectionNameEquals(section, section_name)) {
            continue;
        }

        const std::size_t offset = section.VirtualAddress;
        const std::size_t requested_size = section.Misc.VirtualSize;
        if (offset >= image_size || requested_size > image_size - offset) {
            return {SignatureScanStatus::malformed_image, nullptr, total_matches};
        }

        const auto section_bytes = std::span(image + offset, requested_size);
        const auto match = ScanUnique(section_bytes, pattern);
        if (match.status == SignatureScanStatus::invalid_pattern) {
            return match;
        }
        if (match.status == SignatureScanStatus::ambiguous) {
            return {SignatureScanStatus::ambiguous, nullptr, total_matches + match.matches};
        }
        if (match.status != SignatureScanStatus::found) {
            continue;
        }
        ++total_matches;
        if (total_matches > 1) {
            return {SignatureScanStatus::ambiguous, nullptr, total_matches};
        }
        unique = match.address;
    }

    return unique != nullptr
        ? SignatureScanResult{SignatureScanStatus::found, unique, 1}
        : SignatureScanResult{SignatureScanStatus::not_found, nullptr, 0};
}

std::string_view SignatureScanStatusName(const SignatureScanStatus status) noexcept {
    switch (status) {
        case SignatureScanStatus::found:
            return "found";
        case SignatureScanStatus::not_found:
            return "not_found";
        case SignatureScanStatus::ambiguous:
            return "ambiguous";
        case SignatureScanStatus::invalid_pattern:
            return "invalid_pattern";
        case SignatureScanStatus::invalid_module:
            return "invalid_module";
        case SignatureScanStatus::malformed_image:
            return "malformed_image";
    }
    return "unknown";
}

}  // namespace ds2::modding
