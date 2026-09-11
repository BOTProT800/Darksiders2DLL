#include "byte_storage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ds2::modding {

struct ByteStorageAccess final {
    [[nodiscard]] static std::shared_ptr<const ByteStorage> Create(
        std::vector<std::byte> bytes,
        const Sha256Digest& digest) {
        return std::shared_ptr<const ByteStorage>(
            new ByteStorage(std::move(bytes), digest));
    }
};

namespace {

class FileHandle final {
public:
    explicit FileHandle(const HANDLE handle) noexcept : value_(handle) {}
    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

ByteLoadResult Failure(
    const ByteLoadError error,
    const DWORD win32_error,
    std::wstring detail) {
    ByteLoadResult result;
    result.error = error;
    result.win32_error = win32_error;
    result.detail = std::move(detail);
    return result;
}

bool EqualsOrdinalIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool IsStrictlyContainedBy(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) noexcept {
    auto root_component = root.begin();
    auto candidate_component = candidate.begin();
    for (; root_component != root.end(); ++root_component, ++candidate_component) {
        if (candidate_component == candidate.end() ||
            !EqualsOrdinalIgnoreCase(root_component->native(), candidate_component->native())) {
            return false;
        }
    }
    return candidate_component != candidate.end();
}

bool GetNormalizedFinalPath(
    const HANDLE handle,
    std::filesystem::path& path,
    DWORD& error) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD capacity = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (capacity == 0) {
        error = GetLastError();
        return false;
    }

    // A rename can change the required length between calls. Retry with the
    // newly reported capacity instead of accepting a truncated final path.
    for (;;) {
        std::wstring buffer(static_cast<std::size_t>(capacity), L'\0');
        const DWORD written = GetFinalPathNameByHandleW(
            handle, buffer.data(), capacity, flags);
        if (written == 0) {
            error = GetLastError();
            return false;
        }
        if (written < capacity) {
            buffer.resize(static_cast<std::size_t>(written));
            path = std::filesystem::path(std::move(buffer));
            error = ERROR_SUCCESS;
            return true;
        }
        if (written == (std::numeric_limits<DWORD>::max)()) {
            error = ERROR_FILENAME_EXCED_RANGE;
            return false;
        }
        capacity = written + 1;
    }
}

ByteLoadResult ValidateAssetHandle(const HANDLE file) {
    SetLastError(ERROR_SUCCESS);
    const DWORD file_type = GetFileType(file);
    if (file_type != FILE_TYPE_DISK) {
        const DWORD type_error = file_type == FILE_TYPE_UNKNOWN
            ? GetLastError() : ERROR_INVALID_DATA;
        return Failure(
            ByteLoadError::not_regular_file,
            type_error == ERROR_SUCCESS ? ERROR_INVALID_DATA : type_error,
            L"asset handle is not a disk file");
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            file, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return Failure(ByteLoadError::metadata_failed, GetLastError(), L"cannot read file attributes");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Failure(ByteLoadError::reparse_point, ERROR_REPARSE_TAG_INVALID, L"asset is a reparse point");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return Failure(ByteLoadError::not_regular_file, ERROR_DIRECTORY, L"asset path is a directory");
    }
    return {};
}

ByteLoadResult ReadAssetHandle(
    const HANDLE file,
    const std::uint64_t max_file_size) {
    const auto validated = ValidateAssetHandle(file);
    if (validated.error != ByteLoadError::none) {
        return validated;
    }

    LARGE_INTEGER signed_size{};
    if (!GetFileSizeEx(file, &signed_size)) {
        return Failure(ByteLoadError::metadata_failed, GetLastError(), L"cannot determine asset size");
    }
    if (signed_size.QuadPart < 0) {
        return Failure(ByteLoadError::metadata_failed, ERROR_INVALID_DATA, L"asset has a negative size");
    }
    const auto size = static_cast<std::uint64_t>(signed_size.QuadPart);
    if (size == 0) {
        return Failure(ByteLoadError::empty_file, ERROR_HANDLE_EOF, L"empty assets are not valid overrides");
    }
    if (size > max_file_size || size > (std::numeric_limits<std::size_t>::max)()) {
        return Failure(ByteLoadError::file_too_large, ERROR_FILE_TOO_LARGE, L"asset exceeds the configured size limit");
    }

    std::vector<std::byte> bytes;
    if (static_cast<std::size_t>(size) > bytes.max_size()) {
        return Failure(ByteLoadError::file_too_large, ERROR_FILE_TOO_LARGE, L"asset exceeds the buffer size limit");
    }
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        return Failure(ByteLoadError::allocation_failed, ERROR_NOT_ENOUGH_MEMORY, L"asset buffer allocation failed");
    } catch (const std::length_error&) {
        return Failure(ByteLoadError::allocation_failed, ERROR_NOT_ENOUGH_MEMORY, L"asset buffer length is not representable");
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD bytes_read = 0;
        if (!ReadFile(file, bytes.data() + offset, chunk, &bytes_read, nullptr)) {
            return Failure(ByteLoadError::read_failed, GetLastError(), L"asset read failed");
        }
        if (bytes_read == 0) {
            return Failure(ByteLoadError::read_failed, ERROR_HANDLE_EOF, L"asset was truncated while reading");
        }
        if (static_cast<std::size_t>(bytes_read) > remaining) {
            return Failure(ByteLoadError::read_failed, ERROR_INVALID_DATA, L"asset read exceeded its buffer");
        }
        offset += static_cast<std::size_t>(bytes_read);
    }

    const auto digest = ComputeSha256(bytes);
    if (!digest) {
        return Failure(ByteLoadError::hash_failed, digest.win32_error, L"SHA-256 calculation failed");
    }

    ByteLoadResult result;
    try {
        result.storage = ByteStorageAccess::Create(std::move(bytes), digest.digest);
    } catch (const std::bad_alloc&) {
        return Failure(ByteLoadError::allocation_failed, ERROR_NOT_ENOUGH_MEMORY, L"asset object allocation failed");
    }
    return result;
}

}  // namespace

ByteStorage::ByteStorage(std::vector<std::byte> bytes, const Sha256Digest digest)
    : bytes_(std::move(bytes)), sha256_(digest), sha256_hex_(Sha256HexWide(digest)) {}

ByteLoadResult LoadByteStorage(
    const std::filesystem::path& path,
    const std::uint64_t max_file_size) {
    if (path.empty() || max_file_size == 0) {
        return Failure(ByteLoadError::invalid_path, ERROR_INVALID_PARAMETER, L"invalid asset path or size limit");
    }

    const FileHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return Failure(ByteLoadError::open_failed, GetLastError(), L"CreateFileW failed");
    }

    return ReadAssetHandle(file.get(), max_file_size);
}

ByteLoadResult LoadByteStorageUnderRoot(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::uint64_t max_file_size) {
    if (root.empty() || path.empty() || max_file_size == 0) {
        return Failure(
            ByteLoadError::invalid_path, ERROR_INVALID_PARAMETER,
            L"invalid root, asset path, or size limit");
    }

    const FileHandle root_handle(CreateFileW(
        root.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (root_handle.get() == INVALID_HANDLE_VALUE) {
        return Failure(ByteLoadError::open_failed, GetLastError(), L"cannot open asset root");
    }

    FILE_ATTRIBUTE_TAG_INFO root_attributes{};
    if (!GetFileInformationByHandleEx(
            root_handle.get(), FileAttributeTagInfo,
            &root_attributes, sizeof(root_attributes))) {
        return Failure(ByteLoadError::metadata_failed, GetLastError(), L"cannot read root attributes");
    }
    if ((root_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Failure(ByteLoadError::reparse_point, ERROR_REPARSE_TAG_INVALID, L"asset root is a reparse point");
    }
    if ((root_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Failure(ByteLoadError::not_regular_file, ERROR_DIRECTORY, L"asset root is not a directory");
    }

    const FileHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return Failure(ByteLoadError::open_failed, GetLastError(), L"cannot open asset under root");
    }

    const auto validated = ValidateAssetHandle(file.get());
    if (validated.error != ByteLoadError::none) {
        return validated;
    }

    std::filesystem::path final_root;
    std::filesystem::path final_file;
    DWORD final_path_error = ERROR_SUCCESS;
    if (!GetNormalizedFinalPath(root_handle.get(), final_root, final_path_error)) {
        return Failure(ByteLoadError::final_path_failed, final_path_error, L"cannot resolve final root path");
    }
    if (!GetNormalizedFinalPath(file.get(), final_file, final_path_error)) {
        return Failure(ByteLoadError::final_path_failed, final_path_error, L"cannot resolve final asset path");
    }
    if (!IsStrictlyContainedBy(final_root, final_file)) {
        return Failure(ByteLoadError::outside_root, ERROR_ACCESS_DENIED, L"final asset path is outside root");
    }

    return ReadAssetHandle(file.get(), max_file_size);
}

std::wstring_view ByteLoadErrorName(const ByteLoadError error) noexcept {
    switch (error) {
    case ByteLoadError::none:
        return L"none";
    case ByteLoadError::invalid_path:
        return L"invalid_path";
    case ByteLoadError::open_failed:
        return L"open_failed";
    case ByteLoadError::reparse_point:
        return L"reparse_point";
    case ByteLoadError::not_regular_file:
        return L"not_regular_file";
    case ByteLoadError::metadata_failed:
        return L"metadata_failed";
    case ByteLoadError::final_path_failed:
        return L"final_path_failed";
    case ByteLoadError::outside_root:
        return L"outside_root";
    case ByteLoadError::empty_file:
        return L"empty_file";
    case ByteLoadError::file_too_large:
        return L"file_too_large";
    case ByteLoadError::allocation_failed:
        return L"allocation_failed";
    case ByteLoadError::read_failed:
        return L"read_failed";
    case ByteLoadError::hash_failed:
        return L"hash_failed";
    }
    return L"unknown";
}

}  // namespace ds2::modding
