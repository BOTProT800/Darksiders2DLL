#include "signature_scan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
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

bool IsReadableProtection(const DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    switch (protection & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool IsImageAllocation(const HMODULE module) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(module, &memory, sizeof(memory)) == 0) {
        return false;
    }
    return memory.State == MEM_COMMIT &&
        memory.Type == MEM_IMAGE &&
        memory.AllocationBase == module &&
        IsReadableProtection(memory.Protect);
}

bool IsReadableImageRange(
    const HMODULE module,
    const std::size_t offset,
    const std::size_t size) noexcept {
    if (size == 0) {
        return true;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    constexpr auto maximum = (std::numeric_limits<std::uintptr_t>::max)();
    if (offset > maximum - base) {
        return false;
    }
    const auto first = base + offset;
    if (size > maximum - first) {
        return false;
    }
    const auto end = first + size;

    auto cursor = first;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &memory,
                sizeof(memory)) == 0) {
            return false;
        }
        if (memory.State != MEM_COMMIT ||
            memory.Type != MEM_IMAGE ||
            memory.AllocationBase != module ||
            !IsReadableProtection(memory.Protect)) {
            return false;
        }

        const auto region_first = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > maximum - region_first) {
            return false;
        }
        const auto region_end = region_first + memory.RegionSize;
        if (cursor < region_first || cursor >= region_end) {
            return false;
        }
        cursor = (std::min)(region_end, end);
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
    if (!IsImageAllocation(module)) {
        return {SignatureScanStatus::invalid_module, nullptr, 0};
    }

    const auto* const image = reinterpret_cast<const std::byte*>(module);
    if (!IsReadableImageRange(module, 0, sizeof(IMAGE_DOS_HEADER))) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }

    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    constexpr std::size_t kNtPrefixSize = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (!IsReadableImageRange(module, nt_offset, kNtPrefixSize)) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > 96 ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }

    if (nt->FileHeader.SizeOfOptionalHeader >
        (std::numeric_limits<std::size_t>::max)() - kNtPrefixSize ||
        nt_offset > (std::numeric_limits<std::size_t>::max)() -
            (kNtPrefixSize + nt->FileHeader.SizeOfOptionalHeader)) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }
    const std::size_t section_table_offset =
        nt_offset + kNtPrefixSize + nt->FileHeader.SizeOfOptionalHeader;
    const std::size_t section_table_size =
        static_cast<std::size_t>(nt->FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (!IsReadableImageRange(
            module,
            nt_offset + kNtPrefixSize,
            nt->FileHeader.SizeOfOptionalHeader) ||
        !IsReadableImageRange(module, section_table_offset, section_table_size)) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }

    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    if (image_size == 0 ||
        nt->OptionalHeader.SizeOfHeaders > image_size ||
        section_table_offset > nt->OptionalHeader.SizeOfHeaders ||
        section_table_size > nt->OptionalHeader.SizeOfHeaders - section_table_offset) {
        return {SignatureScanStatus::malformed_image, nullptr, 0};
    }
    const auto* const sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        image + section_table_offset);
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
        if (!IsReadableImageRange(module, offset, requested_size)) {
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
