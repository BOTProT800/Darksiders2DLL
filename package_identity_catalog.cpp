#include "package_identity_catalog.h"

#include "byte_storage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ds2::modding {
namespace {

constexpr std::size_t kObpkHeaderSize = 5 + 6 * sizeof(std::uint32_t) + 1;
constexpr std::size_t kMaximumManifestPackCount = 256;

template <typename Unsigned>
[[nodiscard]] bool CheckedAdd(
    const Unsigned left,
    const Unsigned right,
    Unsigned& sum) noexcept {
    static_assert((std::numeric_limits<Unsigned>::is_integer));
    static_assert(!(std::numeric_limits<Unsigned>::is_signed));
    if (right > (std::numeric_limits<Unsigned>::max)() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

template <typename Unsigned>
[[nodiscard]] bool CheckedMultiply(
    const Unsigned left,
    const Unsigned right,
    Unsigned& product) noexcept {
    static_assert((std::numeric_limits<Unsigned>::is_integer));
    static_assert(!(std::numeric_limits<Unsigned>::is_signed));
    if (left != 0 && right > (std::numeric_limits<Unsigned>::max)() / left) {
        return false;
    }
    product = left * right;
    return true;
}

class SpanReader final {
public:
    explicit SpanReader(const std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] bool Seek(const std::size_t position) noexcept {
        if (position > bytes_.size()) {
            return false;
        }
        position_ = position;
        return true;
    }

    [[nodiscard]] bool Skip(const std::size_t count) noexcept {
        std::size_t next = 0;
        return CheckedAdd(position_, count, next) && Seek(next);
    }

    [[nodiscard]] bool ReadU8(std::uint8_t& value) noexcept {
        if (position_ >= bytes_.size()) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(bytes_[position_]);
        ++position_;
        return true;
    }

    [[nodiscard]] bool ReadU16(std::uint16_t& value) noexcept {
        std::uint64_t temporary = 0;
        if (!ReadUnsigned(2, temporary)) {
            return false;
        }
        value = static_cast<std::uint16_t>(temporary);
        return true;
    }

    [[nodiscard]] bool ReadU32(std::uint32_t& value) noexcept {
        std::uint64_t temporary = 0;
        if (!ReadUnsigned(4, temporary)) {
            return false;
        }
        value = static_cast<std::uint32_t>(temporary);
        return true;
    }

    [[nodiscard]] bool ReadU64(std::uint64_t& value) noexcept {
        return ReadUnsigned(8, value);
    }

    [[nodiscard]] bool ReadString(
        const std::size_t length,
        const std::size_t maximum,
        std::string& value) {
        if (length > maximum || length > Remaining()) {
            return false;
        }
        const auto* const begin = reinterpret_cast<const char*>(
            bytes_.data() + position_);
        value.assign(begin, length);
        position_ += length;
        return true;
    }

    [[nodiscard]] bool ReadSizedString(
        const std::size_t length_bytes,
        const std::size_t maximum,
        std::string& value) {
        std::uint64_t length = 0;
        if (!ReadUnsigned(length_bytes, length) ||
            length > static_cast<std::uint64_t>(maximum) ||
            length > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            return false;
        }
        return ReadString(static_cast<std::size_t>(length), maximum, value);
    }

    [[nodiscard]] std::size_t Position() const noexcept { return position_; }
    [[nodiscard]] std::size_t Remaining() const noexcept {
        return bytes_.size() - position_;
    }

private:
    [[nodiscard]] bool ReadUnsigned(
        const std::size_t byte_count,
        std::uint64_t& value) noexcept {
        if (byte_count == 0 || byte_count > sizeof(value) ||
            byte_count > Remaining()) {
            return false;
        }
        value = 0;
        for (std::size_t index = 0; index < byte_count; ++index) {
            value |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(bytes_[position_ + index]))
                << (index * 8);
        }
        position_ += byte_count;
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t position_{};
};

class PackageFile final {
public:
    PackageFile() noexcept = default;
    ~PackageFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    PackageFile(const PackageFile&) = delete;
    PackageFile& operator=(const PackageFile&) = delete;

    [[nodiscard]] bool Open(
        const std::filesystem::path& path,
        DWORD& error,
        std::wstring& detail) {
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            detail = L"cannot open media.upak";
            return false;
        }

        SetLastError(ERROR_SUCCESS);
        const DWORD file_type = GetFileType(handle_);
        if (file_type != FILE_TYPE_DISK) {
            error = file_type == FILE_TYPE_UNKNOWN
                ? GetLastError() : ERROR_INVALID_DATA;
            if (error == ERROR_SUCCESS) {
                error = ERROR_INVALID_DATA;
            }
            detail = L"media.upak is not a disk file";
            return false;
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                handle_, FileAttributeTagInfo,
                &attributes, sizeof(attributes))) {
            error = GetLastError();
            detail = L"cannot inspect media.upak attributes";
            return false;
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            error = ERROR_REPARSE_TAG_INVALID;
            detail = L"media.upak is a reparse point or directory";
            return false;
        }

        LARGE_INTEGER signed_size{};
        if (!GetFileSizeEx(handle_, &signed_size) || signed_size.QuadPart < 0) {
            error = GetLastError();
            if (error == ERROR_SUCCESS) {
                error = ERROR_INVALID_DATA;
            }
            detail = L"cannot determine media.upak size";
            return false;
        }
        size_ = static_cast<std::uint64_t>(signed_size.QuadPart);
        error = ERROR_SUCCESS;
        return true;
    }

    [[nodiscard]] bool ReadAt(
        const std::uint64_t offset,
        const std::span<std::byte> destination,
        DWORD& error) noexcept {
        std::uint64_t end = 0;
        if (!CheckedAdd(offset, static_cast<std::uint64_t>(destination.size()), end) ||
            end > size_ ||
            offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
            error = ERROR_HANDLE_EOF;
            return false;
        }

        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
            error = GetLastError();
            return false;
        }

        std::size_t copied = 0;
        while (copied < destination.size()) {
            const std::size_t remaining = destination.size() - copied;
            const DWORD chunk = static_cast<DWORD>((std::min)(
                remaining,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD bytes_read = 0;
            if (!ReadFile(
                    handle_, destination.data() + copied,
                    chunk, &bytes_read, nullptr)) {
                error = GetLastError();
                return false;
            }
            if (bytes_read == 0 || bytes_read > remaining) {
                error = ERROR_HANDLE_EOF;
                return false;
            }
            copied += static_cast<std::size_t>(bytes_read);
        }
        error = ERROR_SUCCESS;
        return true;
    }

    [[nodiscard]] std::uint64_t Size() const noexcept { return size_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::uint64_t size_{};
};

struct ManifestSegment final {
    CanonicalVirtualPath virtual_path;
    std::uint64_t offset{};
    std::uint64_t size{};
};

struct RequestedMatch final {
    const CanonicalVirtualPath* requested{};
    std::optional<PackageIdentityEntry> entry;
    bool ambiguous{};
};

[[nodiscard]] bool IdentityLess(
    const PackageResourceIdentity& left,
    const PackageResourceIdentity& right) noexcept {
    if (left.package_base != right.package_base) {
        return left.package_base < right.package_base;
    }
    if (left.member_table_offset != right.member_table_offset) {
        return left.member_table_offset < right.member_table_offset;
    }
    return left.member_ordinal < right.member_ordinal;
}

[[nodiscard]] bool EqualsAsciiIgnoreCase(
    const std::string_view left,
    const std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](const unsigned char value) noexcept {
            return value >= 'A' && value <= 'Z'
                ? static_cast<unsigned char>(value + ('a' - 'A'))
                : value;
        };
        if (lower(static_cast<unsigned char>(left[index])) !=
            lower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool EndsWithOrdinalIgnoreCase(
    const std::wstring_view value,
    const std::wstring_view suffix) noexcept {
    if (suffix.empty() || value.size() < suffix.size() ||
        suffix.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const auto tail = value.substr(value.size() - suffix.size());
    return CompareStringOrdinal(
               tail.data(), static_cast<int>(tail.size()),
               suffix.data(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::wstring ExtensionForType(const std::uint32_t type_id) {
    switch (type_id) {
    case 0: return L".bod";
    case 1: return L".o3d";
    case 2: return L".2";
    case 3: return L".3";
    case 4: return L".4";
    case 5: return L".bmat";
    case 6: return L".dds";
    case 7: return L".tfnt";
    case 8: return L".anm";
    case 9: return L".bnk";
    case 11: return L".smf";
    case 12: return L".gfx";
    case 15: return L".sam";
    case 20: return L".loc";
    default: return L"." + std::to_wstring(type_id);
    }
}

[[nodiscard]] std::optional<std::wstring> Utf8ToWide(
    const std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int input_size = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), input_size, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), input_size, converted.data(), required);
    if (written != required) {
        return std::nullopt;
    }
    return converted;
}

void AddIssue(
    PackageIdentityCatalogBuildResult& result,
    const PackageIdentityCatalogOptions& options,
    const PackageIdentityCatalogIssueCode code,
    std::wstring virtual_path,
    std::wstring detail) {
    if (result.issues.size() >= options.max_issue_count) {
        if (result.dropped_issue_count !=
            (std::numeric_limits<std::size_t>::max)()) {
            ++result.dropped_issue_count;
        }
        return;
    }
    result.issues.push_back(PackageIdentityCatalogIssue{
        code, std::move(virtual_path), std::move(detail)});
}

bool Fail(
    PackageIdentityCatalogBuildResult& result,
    const PackageIdentityCatalogError error,
    const DWORD system_error,
    std::wstring detail) {
    result.snapshot.reset();
    result.error = error;
    result.system_error = system_error;
    result.detail = std::move(detail);
    return false;
}

[[nodiscard]] bool ParseManifestSegments(
    const std::span<const std::byte> bytes,
    const std::uint64_t package_size,
    const PackageIdentityCatalogOptions& options,
    std::vector<ManifestSegment>& segments,
    PackageIdentityCatalogBuildResult& result) {
    SpanReader reader(bytes);
    std::uint32_t version = 0;
    if (!reader.ReadU32(version) || version < 1 || version > 0x100 ||
        !reader.Skip(10 * sizeof(std::uint32_t))) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_manifest,
            ERROR_INVALID_DATA, L"manifest header is invalid");
    }

    std::uint32_t path_count = 0;
    std::uint32_t ignored32 = 0;
    if (!reader.ReadU32(path_count) ||
        path_count > options.max_manifest_paths ||
        !reader.ReadU32(ignored32)) {
        return Fail(
            result, PackageIdentityCatalogError::resource_limit_exceeded,
            ERROR_NOT_ENOUGH_QUOTA, L"manifest path count is invalid or exceeds its limit");
    }

    std::vector<std::string> paths;
    paths.reserve(path_count);
    for (std::uint32_t index = 0; index < path_count; ++index) {
        std::uint64_t ignored64 = 0;
        std::string path;
        if (!reader.ReadU64(ignored64) ||
            !reader.ReadSizedString(4, options.max_string_bytes, path)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_DATA, L"manifest path table is truncated");
        }
        paths.push_back(std::move(path));
    }

    std::uint32_t pack_count = 0;
    if (!reader.ReadU32(pack_count) || pack_count > kMaximumManifestPackCount) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_manifest,
            ERROR_INVALID_DATA, L"manifest package count is invalid");
    }
    std::vector<std::string> packs;
    packs.reserve(pack_count);
    for (std::uint32_t index = 0; index < pack_count; ++index) {
        std::uint8_t ignored8 = 0;
        std::string pack;
        if (!reader.ReadU8(ignored8) ||
            !reader.ReadSizedString(2, options.max_string_bytes, pack)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_DATA, L"manifest package table is truncated");
        }
        packs.push_back(std::move(pack));
    }

    std::uint32_t asset_count = 0;
    if (!reader.ReadU32(asset_count) ||
        asset_count > options.max_manifest_assets) {
        return Fail(
            result, PackageIdentityCatalogError::resource_limit_exceeded,
            ERROR_NOT_ENOUGH_QUOTA, L"manifest asset count exceeds its limit");
    }
    segments.reserve((std::min)(
        static_cast<std::size_t>(asset_count), options.max_segments));
    for (std::uint32_t index = 0; index < asset_count; ++index) {
        std::string filename;
        std::uint64_t ignored64 = 0;
        std::uint16_t ignored16 = 0;
        std::uint16_t path_index = 0;
        std::uint8_t alias_count = 0;
        if (!reader.ReadSizedString(1, options.max_string_bytes, filename) ||
            !reader.ReadU64(ignored64) ||
            !reader.ReadU16(ignored16) ||
            !reader.ReadU16(path_index) ||
            !reader.ReadU8(alias_count)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_DATA, L"manifest asset record is truncated");
        }
        std::size_t alias_bytes = 0;
        if (!CheckedMultiply(
                static_cast<std::size_t>(alias_count),
                sizeof(std::uint16_t), alias_bytes) ||
            !reader.Skip(alias_bytes)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_DATA, L"manifest alias table is truncated");
        }

        std::uint8_t ignored8 = 0;
        std::uint8_t pack_index = 0;
        std::uint8_t ignored_after_pack1 = 0;
        std::uint8_t ignored_after_pack2 = 0;
        if (!reader.ReadU8(ignored8) ||
            !reader.ReadU8(pack_index) ||
            !reader.ReadU8(ignored_after_pack1) ||
            !reader.ReadU8(ignored_after_pack2)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_DATA, L"manifest asset package fields are truncated");
        }

        std::uint64_t offset = 0;
        if (version >= 0x0D) {
            if (!reader.ReadU64(offset)) {
                return Fail(
                    result, PackageIdentityCatalogError::invalid_manifest,
                    ERROR_INVALID_DATA, L"manifest 64-bit asset offset is truncated");
            }
        } else {
            std::uint32_t offset32 = 0;
            if (!reader.ReadU32(offset32)) {
                return Fail(
                    result, PackageIdentityCatalogError::invalid_manifest,
                    ERROR_INVALID_DATA, L"manifest 32-bit asset offset is truncated");
            }
            offset = offset32;
        }

        if (pack_index == 0xFF) {
            continue;
        }
        if (pack_index >= packs.size() || path_index >= paths.size()) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_DATA, L"manifest asset refers to an invalid table index");
        }
        if (!EqualsAsciiIgnoreCase(packs[pack_index], "media")) {
            continue;
        }
        if (segments.size() >= options.max_segments) {
            return Fail(
                result, PackageIdentityCatalogError::resource_limit_exceeded,
                ERROR_NOT_ENOUGH_QUOTA, L"media segment count exceeds its limit");
        }

        std::string combined = packs[pack_index];
        if (!paths[path_index].empty()) {
            combined.push_back('/');
            combined.append(paths[path_index]);
        }
        if (!filename.empty()) {
            combined.push_back('/');
            combined.append(filename);
        }
        const auto wide = Utf8ToWide(combined);
        if (!wide) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_NO_UNICODE_TRANSLATION, L"manifest contains invalid UTF-8");
        }
        const auto normalized = NormalizeVirtualPath(*wide);
        if (!normalized) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_NAME, L"manifest contains an unsafe media segment path");
        }
        segments.push_back(ManifestSegment{
            std::move(*normalized.path), offset, 0});
    }

    std::sort(
        segments.begin(), segments.end(),
        [](const ManifestSegment& left, const ManifestSegment& right) {
            return left.offset < right.offset;
        });
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::uint64_t next = index + 1 < segments.size()
            ? segments[index + 1].offset : package_size;
        if (segments[index].offset >= next || next > package_size) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_manifest,
                ERROR_INVALID_DATA, L"media segment offsets overlap or leave the package range");
        }
        segments[index].size = next - segments[index].offset;
    }
    return true;
}

[[nodiscard]] bool ParseRequestedSegment(
    PackageFile& package,
    const ManifestSegment& segment,
    const std::span<const CanonicalVirtualPath* const> requested_paths,
    const PackageIdentityCatalogOptions& options,
    std::vector<PackageIdentityEntry>& entries,
    PackageIdentityCatalogBuildResult& result) {
    if (segment.size < kObpkHeaderSize) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"a requested OBPK segment is shorter than its header");
    }
    std::array<std::byte, kObpkHeaderSize> header_bytes{};
    DWORD read_error = ERROR_SUCCESS;
    if (!package.ReadAt(segment.offset, header_bytes, read_error)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            read_error, L"cannot read a requested OBPK header");
    }

    SpanReader header(header_bytes);
    std::string magic;
    if (!header.ReadString(5, 5, magic) || magic != std::string("OBPK\0", 5)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"a requested media segment is not OBPK");
    }
    std::array<std::uint32_t, 6> fields{};
    for (auto& field : fields) {
        if (!header.ReadU32(field)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_INVALID_DATA, L"a requested OBPK header is truncated");
        }
    }
    std::uint8_t padding = 0;
    if (!header.ReadU8(padding)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"a requested OBPK header padding byte is missing");
    }
    static_cast<void>(padding);

    const std::uint32_t version = fields[0];
    const std::uint32_t table_offset = fields[1];
    const std::uint32_t table_size = fields[2];
    const std::uint32_t names_end = fields[3];
    const std::uint32_t metadata_size = fields[4];
    const std::uint32_t payload_offset = fields[5];
    std::uint64_t metadata_end = 0;
    if (version != 9 || table_offset < kObpkHeaderSize || table_offset >= names_end ||
        names_end - table_offset != table_size ||
        !CheckedAdd(
            static_cast<std::uint64_t>(names_end),
            static_cast<std::uint64_t>(metadata_size), metadata_end) ||
        metadata_end > payload_offset ||
        payload_offset < kObpkHeaderSize ||
        payload_offset > segment.size - sizeof(std::uint32_t) ||
        payload_offset > options.max_segment_metadata_bytes) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"a requested OBPK has invalid metadata offsets");
    }
    std::vector<std::byte> metadata(static_cast<std::size_t>(payload_offset));
    if (!package.ReadAt(segment.offset, metadata, read_error)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            read_error, L"cannot read requested OBPK metadata");
    }
    SpanReader reader(metadata);
    if (!reader.Seek(table_offset)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"requested OBPK table offset is out of range");
    }

    std::uint32_t group_count = 0;
    std::uint32_t file_count = 0;
    std::uint32_t hash_table_size = 0;
    std::uint32_t ignored32 = 0;
    if (!reader.ReadU32(group_count) ||
        !reader.ReadU32(file_count) ||
        !reader.ReadU32(hash_table_size) ||
        !reader.ReadU32(ignored32) ||
        group_count > 100'000 ||
        file_count > options.max_members_per_segment ||
        !reader.Skip(hash_table_size) ||
        !reader.Skip(4 * sizeof(std::uint32_t))) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"requested OBPK member header is invalid");
    }

    std::vector<std::uint32_t> type_ids;
    type_ids.reserve(file_count);
    for (std::uint32_t group = 0; group < group_count; ++group) {
        std::uint32_t type_id = 0;
        std::uint32_t member_count = 0;
        if (!reader.ReadU32(type_id) ||
            !reader.ReadU32(member_count) ||
            !reader.ReadU32(ignored32) ||
            !reader.ReadU32(ignored32) ||
            member_count > file_count - type_ids.size() ||
            !reader.Skip(member_count)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_INVALID_DATA, L"requested OBPK type groups are invalid");
        }
        type_ids.insert(type_ids.end(), member_count, type_id);
    }
    if (type_ids.size() != file_count || reader.Position() > names_end) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"requested OBPK type groups do not match its member count");
    }

    std::vector<std::string> names(file_count);
    std::uint8_t has_names = 0;
    if (!reader.ReadU8(has_names) || has_names > 1) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"requested OBPK name-table flag is invalid");
    }
    if (has_names != 0) {
        std::uint32_t maximum_name_length = 0;
        if (!reader.ReadU32(maximum_name_length) ||
            maximum_name_length > options.max_string_bytes) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_INVALID_DATA, L"requested OBPK name-table limit is invalid");
        }
        for (auto& name : names) {
            std::uint32_t name_length = 0;
            if (!reader.ReadU32(ignored32) ||
                !reader.ReadU32(ignored32) ||
                !reader.ReadU32(name_length) ||
                name_length > maximum_name_length ||
                !reader.ReadString(
                    name_length, options.max_string_bytes,
                    name)) {
                return Fail(
                    result, PackageIdentityCatalogError::invalid_package,
                    ERROR_INVALID_DATA, L"requested OBPK name table is invalid");
            }
        }
    }
    if (reader.Position() != names_end) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA,
            L"requested OBPK name table does not end at member metadata");
    }

    if (file_count == 0) {
        if (metadata_size != 0) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_INVALID_DATA,
                L"an empty requested OBPK has unexpected member metadata");
        }
        for (const auto* const path : requested_paths) {
            AddIssue(
                result, options,
                PackageIdentityCatalogIssueCode::member_not_found,
                path->display,
                L"the named OBPK segment contains no members");
        }
        return true;
    }

    std::size_t size_table_bytes = 0;
    if (!CheckedMultiply(
            static_cast<std::size_t>(file_count),
            sizeof(std::uint32_t), size_table_bytes) ||
        size_table_bytes > metadata_size ||
        metadata_end > (std::numeric_limits<std::size_t>::max)()) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"requested OBPK size table is invalid");
    }
    const auto final_table_offset =
        static_cast<std::size_t>(metadata_end) - size_table_bytes;
    if (final_table_offset < names_end || !reader.Seek(names_end)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"requested OBPK member metadata is out of range");
    }

    std::vector<std::uint32_t> member_sizes(file_count);
    for (auto& member_size : member_sizes) {
        std::uint32_t extra_size = 0;
        if (!reader.ReadU32(member_size) ||
            !reader.ReadU32(extra_size) ||
            !reader.Skip(extra_size) ||
            reader.Position() > final_table_offset) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_INVALID_DATA, L"requested OBPK member metadata is invalid");
        }
    }
    if (reader.Position() != final_table_offset ||
        !reader.Seek(final_table_offset)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA,
            L"requested OBPK member metadata does not end at its final table");
    }

    std::vector<std::uint32_t> final_values(file_count);
    for (auto& value : final_values) {
        if (!reader.ReadU32(value)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_INVALID_DATA, L"requested OBPK final table is truncated");
        }
    }

    const bool sizes_match = std::equal(
        member_sizes.begin(), member_sizes.end(), final_values.begin());

    std::uint64_t total_uncompressed_size = 0;
    for (const auto member_size : member_sizes) {
        std::uint64_t next_size = 0;
        if (!CheckedAdd(
                total_uncompressed_size,
                static_cast<std::uint64_t>(member_size), next_size)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_ARITHMETIC_OVERFLOW,
                L"requested OBPK member sizes overflow their address space");
        }
        total_uncompressed_size = next_size;
    }

    std::array<std::byte, sizeof(std::uint32_t)> declared_bytes{};
    std::uint64_t declared_position = 0;
    if (!CheckedAdd(
            segment.offset,
            static_cast<std::uint64_t>(payload_offset), declared_position) ||
        !package.ReadAt(declared_position, declared_bytes, read_error)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            read_error, L"cannot read requested OBPK unpacked size");
    }
    SpanReader declared_reader(declared_bytes);
    std::uint32_t declared_unpacked_size = 0;
    if (!declared_reader.ReadU32(declared_unpacked_size)) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA, L"requested OBPK unpacked size is truncated");
    }

    const bool stream_layout = sizes_match &&
        total_uncompressed_size == declared_unpacked_size;
    if (!stream_layout) {
        bool looks_like_member_blocks = false;
        if (final_values.front() == payload_offset) {
            looks_like_member_blocks = true;
            for (std::size_t index = 0;
                 index < final_values.size(); ++index) {
                const std::uint64_t begin = final_values[index];
                const std::uint64_t end = index + 1 < final_values.size()
                    ? final_values[index + 1] : segment.size;
                if (begin >= end || end - begin < 5 || end > segment.size) {
                    return Fail(
                        result, PackageIdentityCatalogError::invalid_package,
                        ERROR_INVALID_DATA,
                        L"a requested per-member OBPK has a truncated block table");
                }
            }
        }
        if (!looks_like_member_blocks && sizes_match) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_INVALID_DATA,
                L"requested OBPK member sizes do not match its unpacked size");
        }
        const auto detail = looks_like_member_blocks
            ? L"the requested asset uses per-member OBPK blocks not yet mapped by the prototype"
            : L"the requested asset uses an unknown OBPK payload layout";
        for (const auto* const path : requested_paths) {
            AddIssue(
                result, options,
                PackageIdentityCatalogIssueCode::unsupported_layout,
                path->display, detail);
        }
        return true;
    }

    if (static_cast<std::uint64_t>(payload_offset) + sizeof(std::uint32_t) >=
        segment.size) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_HANDLE_EOF,
            L"requested single-stream OBPK has no compressed payload bytes");
    }

    std::map<std::wstring, RequestedMatch, std::less<>> matches;
    for (const auto* const path : requested_paths) {
        matches.try_emplace(path->key, RequestedMatch{path, std::nullopt, false});
    }

    std::uint64_t uncompressed_offset = 0;
    for (std::size_t index = 0; index < member_sizes.size(); ++index) {
        const std::uint32_t member_size = member_sizes[index];
        if (!names[index].empty()) {
            const auto wide_name = Utf8ToWide(names[index]);
            if (!wide_name) {
                return Fail(
                    result, PackageIdentityCatalogError::invalid_package,
                    ERROR_NO_UNICODE_TRANSLATION,
                    L"requested OBPK contains an invalid UTF-8 member name");
            }
            std::wstring member_name = *wide_name;
            const auto extension = ExtensionForType(type_ids[index]);
            if (!EndsWithOrdinalIgnoreCase(member_name, extension)) {
                member_name.append(extension);
            }
            std::wstring combined = segment.virtual_path.display;
            combined.push_back(L'/');
            combined.append(member_name);
            const auto normalized = NormalizeVirtualPath(combined);
            if (!normalized) {
                return Fail(
                    result, PackageIdentityCatalogError::invalid_package,
                    ERROR_INVALID_NAME,
                    L"requested OBPK contains an unsafe member name");
            }
            const auto match = matches.find(normalized.path->key);
            if (match != matches.end()) {
                if (match->second.entry.has_value()) {
                    match->second.entry.reset();
                    match->second.ambiguous = true;
                } else if (!match->second.ambiguous) {
                    match->second.entry.emplace(PackageIdentityEntry{
                        PackageResourceIdentity{
                            segment.offset,
                            payload_offset,
                            static_cast<std::uint32_t>(index + 1)},
                        *normalized.path,
                        segment.size,
                        uncompressed_offset,
                        member_size,
                        type_ids[index]});
                }
            }
        }
        std::uint64_t next_offset = 0;
        if (!CheckedAdd(
                uncompressed_offset,
                static_cast<std::uint64_t>(member_size), next_offset)) {
            return Fail(
                result, PackageIdentityCatalogError::invalid_package,
                ERROR_ARITHMETIC_OVERFLOW,
                L"requested OBPK member sizes overflow their address space");
        }
        uncompressed_offset = next_offset;
    }
    if (uncompressed_offset != declared_unpacked_size) {
        return Fail(
            result, PackageIdentityCatalogError::invalid_package,
            ERROR_INVALID_DATA,
            L"requested OBPK member sizes do not match its unpacked size");
    }

    for (auto& [key, match] : matches) {
        static_cast<void>(key);
        if (match.ambiguous) {
            AddIssue(
                result, options,
                PackageIdentityCatalogIssueCode::ambiguous_member,
                match.requested->display,
                L"multiple OBPK members normalize to the requested path");
        } else if (!match.entry.has_value()) {
            AddIssue(
                result, options,
                PackageIdentityCatalogIssueCode::member_not_found,
                match.requested->display,
                L"the named OBPK segment does not contain the requested member");
        } else {
            entries.push_back(std::move(*match.entry));
        }
    }
    return true;
}

}  // namespace

PackageIdentityCatalogSnapshot::PackageIdentityCatalogSnapshot(
    std::vector<PackageIdentityEntry> entries,
    std::vector<std::size_t> path_order) noexcept
    : entries_(std::move(entries)), path_order_(std::move(path_order)) {}

const PackageIdentityEntry* PackageIdentityCatalogSnapshot::Find(
    const PackageResourceIdentity& identity) const noexcept {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), identity,
        [](const PackageIdentityEntry& entry,
           const PackageResourceIdentity& key) noexcept {
            return IdentityLess(entry.identity, key);
        });
    if (found == entries_.end() || !(found->identity == identity)) {
        return nullptr;
    }
    return std::addressof(*found);
}

const PackageIdentityEntry* PackageIdentityCatalogSnapshot::Find(
    const CanonicalVirtualPath& virtual_path) const noexcept {
    return Find(virtual_path.key);
}

const PackageIdentityEntry* PackageIdentityCatalogSnapshot::Find(
    const std::wstring_view canonical_key) const noexcept {
    const auto found = std::lower_bound(
        path_order_.begin(), path_order_.end(), canonical_key,
        [this](const std::size_t index, const std::wstring_view key) noexcept {
            return entries_[index].virtual_path.key < key;
        });
    if (found == path_order_.end() ||
        entries_[*found].virtual_path.key != canonical_key) {
        return nullptr;
    }
    return std::addressof(entries_[*found]);
}

PackageIdentityCatalogBuildResult BuildMediaPackageIdentityCatalog(
    const std::filesystem::path& game_directory,
    const std::span<const CanonicalVirtualPath> requested_paths,
    const PackageIdentityCatalogOptions& options) {
    PackageIdentityCatalogBuildResult result;
    if (game_directory.empty() ||
        options.max_manifest_bytes == 0 ||
        options.max_requested_paths == 0 ||
        options.max_manifest_paths == 0 ||
        options.max_manifest_assets == 0 ||
        options.max_segments == 0 ||
        options.max_members_per_segment == 0 ||
        options.max_segment_metadata_bytes < kObpkHeaderSize ||
        options.max_string_bytes == 0 ||
        options.max_issue_count == 0 ||
        requested_paths.size() > options.max_requested_paths) {
        Fail(
            result, PackageIdentityCatalogError::invalid_argument,
            ERROR_INVALID_PARAMETER, L"invalid package catalog input or limits");
        return result;
    }

    try {
        std::map<std::wstring, CanonicalVirtualPath, std::less<>> requested;
        for (const auto& path : requested_paths) {
            const auto normalized = NormalizeVirtualPath(path.display);
            if (!normalized || normalized.path->key != path.key) {
                Fail(
                    result, PackageIdentityCatalogError::invalid_argument,
                    ERROR_INVALID_NAME,
                    L"requested package path is not a valid canonical path");
                return result;
            }
            requested.try_emplace(path.key, path);
        }

        if (requested.empty()) {
            result.snapshot = std::shared_ptr<const PackageIdentityCatalogSnapshot>(
                new PackageIdentityCatalogSnapshot({}, {}));
            return result;
        }

        const auto media_directory = game_directory / L"media";
        const auto manifest_path = media_directory / L"manifest.bin";
        auto manifest = LoadByteStorage(
            manifest_path, options.max_manifest_bytes);
        if (!manifest) {
            result.error = PackageIdentityCatalogError::manifest_load_failed;
            result.system_error = manifest.win32_error;
            result.detail = std::wstring(ByteLoadErrorName(manifest.error)) +
                L": " + manifest.detail;
            return result;
        }

        PackageFile package;
        DWORD open_error = ERROR_SUCCESS;
        std::wstring open_detail;
        if (!package.Open(
                media_directory / L"media.upak",
                open_error, open_detail)) {
            result.error = PackageIdentityCatalogError::package_open_failed;
            result.system_error = open_error;
            result.detail = std::move(open_detail);
            return result;
        }

        std::vector<ManifestSegment> segments;
        if (!ParseManifestSegments(
                manifest.storage->Bytes(), package.Size(), options,
                segments, result)) {
            return result;
        }

        std::map<std::wstring, std::size_t, std::less<>> segments_by_path;
        for (std::size_t index = 0; index < segments.size(); ++index) {
            const auto [entry, inserted] = segments_by_path.try_emplace(
                segments[index].virtual_path.key, index);
            static_cast<void>(entry);
            if (!inserted) {
                Fail(
                    result, PackageIdentityCatalogError::invalid_manifest,
                    ERROR_DUP_NAME,
                    L"manifest contains duplicate canonical media segment paths");
                return result;
            }
        }

        std::map<std::size_t, std::vector<const CanonicalVirtualPath*>> grouped;
        for (auto& [key, path] : requested) {
            std::wstring parent = key;
            bool found_segment = false;
            for (;;) {
                const auto slash = parent.rfind(L'/');
                if (slash == std::wstring::npos) {
                    break;
                }
                parent.resize(slash);
                const auto segment = segments_by_path.find(parent);
                if (segment != segments_by_path.end()) {
                    grouped[segment->second].push_back(std::addressof(path));
                    found_segment = true;
                    break;
                }
            }
            if (!found_segment) {
                AddIssue(
                    result, options,
                    PackageIdentityCatalogIssueCode::path_not_in_manifest,
                    path.display,
                    L"no named media.upak segment contains the requested path");
            }
        }

        std::vector<PackageIdentityEntry> entries;
        entries.reserve(requested.size());
        for (const auto& [segment_index, paths] : grouped) {
            if (!ParseRequestedSegment(
                    package, segments[segment_index], paths,
                    options, entries, result)) {
                return result;
            }
        }

        std::sort(
            entries.begin(), entries.end(),
            [](const PackageIdentityEntry& left,
               const PackageIdentityEntry& right) noexcept {
                return IdentityLess(left.identity, right.identity);
            });
        for (std::size_t index = 1; index < entries.size(); ++index) {
            if (entries[index - 1].identity == entries[index].identity) {
                Fail(
                    result, PackageIdentityCatalogError::invalid_package,
                    ERROR_DUP_NAME,
                    L"multiple requested paths resolve to one package identity");
                return result;
            }
        }

        std::vector<std::size_t> path_order(entries.size());
        std::iota(path_order.begin(), path_order.end(), std::size_t{0});
        std::sort(
            path_order.begin(), path_order.end(),
            [&entries](const std::size_t left, const std::size_t right) noexcept {
                return entries[left].virtual_path.key <
                    entries[right].virtual_path.key;
            });
        for (std::size_t index = 1; index < path_order.size(); ++index) {
            if (entries[path_order[index - 1]].virtual_path.key ==
                entries[path_order[index]].virtual_path.key) {
                Fail(
                    result, PackageIdentityCatalogError::invalid_package,
                    ERROR_DUP_NAME,
                    L"a requested path resolved to multiple package identities");
                return result;
            }
        }

        result.snapshot = std::shared_ptr<const PackageIdentityCatalogSnapshot>(
            new PackageIdentityCatalogSnapshot(
                std::move(entries), std::move(path_order)));
        return result;
    } catch (const std::bad_alloc&) {
        result.snapshot.reset();
        result.error = PackageIdentityCatalogError::allocation_failed;
        result.system_error = ERROR_NOT_ENOUGH_MEMORY;
        result.detail = L"package identity catalog allocation failed";
        return result;
    } catch (const std::length_error&) {
        result.snapshot.reset();
        result.error = PackageIdentityCatalogError::allocation_failed;
        result.system_error = ERROR_NOT_ENOUGH_MEMORY;
        result.detail = L"package identity catalog length is not representable";
        return result;
    } catch (...) {
        result.snapshot.reset();
        result.error = PackageIdentityCatalogError::invalid_package;
        result.system_error = ERROR_INVALID_DATA;
        result.detail = L"unexpected package identity catalog exception";
        return result;
    }
}

std::wstring_view PackageIdentityCatalogErrorName(
    const PackageIdentityCatalogError error) noexcept {
    switch (error) {
    case PackageIdentityCatalogError::none: return L"none";
    case PackageIdentityCatalogError::invalid_argument: return L"invalid_argument";
    case PackageIdentityCatalogError::manifest_load_failed: return L"manifest_load_failed";
    case PackageIdentityCatalogError::invalid_manifest: return L"invalid_manifest";
    case PackageIdentityCatalogError::package_open_failed: return L"package_open_failed";
    case PackageIdentityCatalogError::invalid_package: return L"invalid_package";
    case PackageIdentityCatalogError::resource_limit_exceeded:
        return L"resource_limit_exceeded";
    case PackageIdentityCatalogError::allocation_failed: return L"allocation_failed";
    }
    return L"unknown";
}

std::wstring_view PackageIdentityCatalogIssueCodeName(
    const PackageIdentityCatalogIssueCode code) noexcept {
    switch (code) {
    case PackageIdentityCatalogIssueCode::path_not_in_manifest:
        return L"path_not_in_manifest";
    case PackageIdentityCatalogIssueCode::member_not_found:
        return L"member_not_found";
    case PackageIdentityCatalogIssueCode::ambiguous_member:
        return L"ambiguous_member";
    case PackageIdentityCatalogIssueCode::unsupported_layout:
        return L"unsupported_layout";
    }
    return L"unknown";
}

}  // namespace ds2::modding
