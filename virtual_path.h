#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ds2::modding {

enum class VirtualPathError {
    none,
    empty,
    too_long,
    invalid_unicode,
    absolute,
    drive_qualified,
    empty_component,
    traversal,
    alternate_data_stream,
    invalid_character,
    trailing_dot_or_space,
    reserved_device_name,
    outside_root,
    filesystem_error,
};

// A validated path relative to a mod root. display keeps the caller's casing
// while normalizing separators; key is NFC-normalized and invariant-lowercase.
struct CanonicalVirtualPath final {
    std::wstring original;
    std::wstring display;
    std::wstring key;

    [[nodiscard]] std::filesystem::path NativeRelativePath() const;
};

struct VirtualPathResult final {
    std::optional<CanonicalVirtualPath> path;
    VirtualPathError error{VirtualPathError::none};

    [[nodiscard]] explicit operator bool() const noexcept {
        return path.has_value();
    }
};

// Accepts only relative asset paths. Both slash styles are recognized, but the
// returned display/key always use '/'. Absolute/UNC/device paths, ADS, '.',
// '..', empty components, Win32 device names and aliasing trailing dots/spaces
// are rejected rather than repaired.
[[nodiscard]] VirtualPathResult NormalizeVirtualPath(std::wstring_view input);

[[nodiscard]] std::wstring_view VirtualPathErrorName(VirtualPathError error) noexcept;

// Resolves a previously validated virtual path and verifies the resulting path
// remains beneath root after canonicalizing any existing prefix. This is a
// containment check, not permission to follow reparse points; file loaders must
// still reject reparse-point entries.
[[nodiscard]] std::optional<std::filesystem::path> ResolveUnderRoot(
    const std::filesystem::path& root,
    const CanonicalVirtualPath& relative,
    VirtualPathError* error = nullptr);

// A mod id is exactly one safe virtual-path component.
[[nodiscard]] bool IsSafeModId(std::wstring_view mod_id);

}  // namespace ds2::modding
