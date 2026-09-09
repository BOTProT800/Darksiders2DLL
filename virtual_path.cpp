#include "virtual_path.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <vector>

#pragma comment(lib, "Normaliz.lib")

namespace ds2::modding {
namespace {

constexpr std::size_t kMaxVirtualPathCharacters = 32'767;

bool IsSeparator(const wchar_t value) noexcept {
    return value == L'/' || value == L'\\';
}

bool IsInvalidWin32Character(const wchar_t value) noexcept {
    return value < 0x20 || value == L'<' || value == L'>' || value == L'"' ||
           value == L'|' || value == L'?' || value == L'*';
}

bool EqualsOrdinalIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }

    return CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

bool IsReservedDeviceName(const std::wstring_view component) noexcept {
    const auto dot = component.find(L'.');
    const auto base = component.substr(0, dot);

    constexpr std::array<std::wstring_view, 8> kExactNames{
        L"con", L"prn", L"aux", L"nul", L"clock$", L"conin$", L"conout$", L"config$"};
    for (const auto name : kExactNames) {
        if (EqualsOrdinalIgnoreCase(base, name)) {
            return true;
        }
    }

    if (base.size() == 4 &&
        (EqualsOrdinalIgnoreCase(base.substr(0, 3), L"com") ||
         EqualsOrdinalIgnoreCase(base.substr(0, 3), L"lpt")) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return true;
    }

    return false;
}

std::optional<std::wstring> NormalizeNfc(const std::wstring_view input) {
    if (input.empty()) {
        return std::wstring{};
    }
    if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }

    const int input_length = static_cast<int>(input.size());
    const int required = NormalizeString(
        NormalizationC, input.data(), input_length, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }

    std::wstring normalized(static_cast<std::size_t>(required), L'\0');
    const int written = NormalizeString(
        NormalizationC, input.data(), input_length, normalized.data(), required);
    if (written <= 0) {
        return std::nullopt;
    }
    normalized.resize(static_cast<std::size_t>(written));
    return normalized;
}

std::optional<std::wstring> InvariantLower(const std::wstring_view input) {
    if (input.empty()) {
        return std::wstring{};
    }
    if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }

    const int input_length = static_cast<int>(input.size());
    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        input.data(),
        input_length,
        nullptr,
        0,
        nullptr,
        nullptr,
        0);
    if (required <= 0) {
        return std::nullopt;
    }

    std::wstring lowered(static_cast<std::size_t>(required), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        input.data(),
        input_length,
        lowered.data(),
        required,
        nullptr,
        nullptr,
        0);
    if (written <= 0) {
        return std::nullopt;
    }
    lowered.resize(static_cast<std::size_t>(written));
    return lowered;
}

void SetError(VirtualPathError* const target, const VirtualPathError value) noexcept {
    if (target != nullptr) {
        *target = value;
    }
}

bool PathComponentEquals(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
    return EqualsOrdinalIgnoreCase(left.native(), right.native());
}

bool IsContainedBy(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) noexcept {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || !PathComponentEquals(*root_it, *candidate_it)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::filesystem::path CanonicalVirtualPath::NativeRelativePath() const {
    std::wstring native = display;
    std::replace(native.begin(), native.end(), L'/', L'\\');
    return std::filesystem::path(std::move(native));
}

VirtualPathResult NormalizeVirtualPath(const std::wstring_view input) {
    VirtualPathResult result;
    if (input.empty()) {
        result.error = VirtualPathError::empty;
        return result;
    }
    if (input.size() > kMaxVirtualPathCharacters ||
        input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        result.error = VirtualPathError::too_long;
        return result;
    }
    if (IsSeparator(input.front())) {
        result.error = VirtualPathError::absolute;
        return result;
    }
    if (input.size() >= 2 && input[1] == L':') {
        result.error = VirtualPathError::drive_qualified;
        return result;
    }

    std::wstring display;
    display.reserve(input.size());
    std::size_t component_start = 0;
    for (std::size_t index = 0; index <= input.size(); ++index) {
        const bool at_end = index == input.size();
        if (!at_end && !IsSeparator(input[index])) {
            continue;
        }

        const auto component = input.substr(component_start, index - component_start);
        if (component.empty()) {
            result.error = VirtualPathError::empty_component;
            return result;
        }
        if (component == L"." || component == L"..") {
            result.error = VirtualPathError::traversal;
            return result;
        }
        if (component.back() == L'.' || component.back() == L' ') {
            result.error = VirtualPathError::trailing_dot_or_space;
            return result;
        }
        if (IsReservedDeviceName(component)) {
            result.error = VirtualPathError::reserved_device_name;
            return result;
        }

        for (const wchar_t character : component) {
            if (character == L'\0' || IsInvalidWin32Character(character)) {
                result.error = VirtualPathError::invalid_character;
                return result;
            }
            if (character == L':') {
                result.error = VirtualPathError::alternate_data_stream;
                return result;
            }
        }

        if (!display.empty()) {
            display.push_back(L'/');
        }
        display.append(component);
        component_start = index + 1;
    }

    const auto normalized = NormalizeNfc(display);
    if (!normalized.has_value()) {
        result.error = VirtualPathError::invalid_unicode;
        return result;
    }
    const auto lowered = InvariantLower(*normalized);
    if (!lowered.has_value()) {
        result.error = VirtualPathError::invalid_unicode;
        return result;
    }

    result.path = CanonicalVirtualPath{
        std::wstring(input), std::move(display), std::move(*lowered)};
    result.error = VirtualPathError::none;
    return result;
}

std::wstring_view VirtualPathErrorName(const VirtualPathError error) noexcept {
    switch (error) {
        case VirtualPathError::none:
            return L"none";
        case VirtualPathError::empty:
            return L"empty";
        case VirtualPathError::too_long:
            return L"too_long";
        case VirtualPathError::invalid_unicode:
            return L"invalid_unicode";
        case VirtualPathError::absolute:
            return L"absolute";
        case VirtualPathError::drive_qualified:
            return L"drive_qualified";
        case VirtualPathError::empty_component:
            return L"empty_component";
        case VirtualPathError::traversal:
            return L"traversal";
        case VirtualPathError::alternate_data_stream:
            return L"alternate_data_stream";
        case VirtualPathError::invalid_character:
            return L"invalid_character";
        case VirtualPathError::trailing_dot_or_space:
            return L"trailing_dot_or_space";
        case VirtualPathError::reserved_device_name:
            return L"reserved_device_name";
        case VirtualPathError::outside_root:
            return L"outside_root";
        case VirtualPathError::filesystem_error:
            return L"filesystem_error";
    }
    return L"unknown";
}

std::optional<std::filesystem::path> ResolveUnderRoot(
    const std::filesystem::path& root,
    const CanonicalVirtualPath& relative,
    VirtualPathError* const error) {
    SetError(error, VirtualPathError::none);
    if (root.empty() || root.is_relative()) {
        SetError(error, VirtualPathError::filesystem_error);
        return std::nullopt;
    }

    std::error_code filesystem_error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, filesystem_error);
    if (filesystem_error) {
        SetError(error, VirtualPathError::filesystem_error);
        return std::nullopt;
    }

    const auto candidate = canonical_root / relative.NativeRelativePath();
    const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, filesystem_error);
    if (filesystem_error) {
        SetError(error, VirtualPathError::filesystem_error);
        return std::nullopt;
    }
    if (!IsContainedBy(canonical_root, canonical_candidate)) {
        SetError(error, VirtualPathError::outside_root);
        return std::nullopt;
    }
    return canonical_candidate;
}

bool IsSafeModId(const std::wstring_view mod_id) {
    const auto normalized = NormalizeVirtualPath(mod_id);
    return normalized &&
           normalized.path->display.find(L'/') == std::wstring::npos;
}

}  // namespace ds2::modding
