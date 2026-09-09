#include "byte_storage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace ds2::modding {
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

}  // namespace

ByteStorage::ByteStorage(std::vector<std::byte> bytes, const Sha256Digest digest)
    : bytes_(std::move(bytes)), sha256_(digest), sha256_hex_(Sha256HexWide(digest)) {}

ByteLoadResult LoadByteStorage(
    const std::filesystem::path& path,
    const std::uint64_t max_file_size) {
    if (path.empty()) {
        return Failure(ByteLoadError::invalid_path, ERROR_INVALID_PARAMETER, L"empty asset path");
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

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return Failure(ByteLoadError::metadata_failed, GetLastError(), L"cannot read file attributes");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Failure(ByteLoadError::reparse_point, ERROR_REPARSE_TAG_INVALID, L"asset is a reparse point");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return Failure(ByteLoadError::not_regular_file, ERROR_DIRECTORY, L"asset path is a directory");
    }

    LARGE_INTEGER signed_size{};
    if (!GetFileSizeEx(file.get(), &signed_size) || signed_size.QuadPart < 0) {
        return Failure(ByteLoadError::metadata_failed, GetLastError(), L"cannot determine asset size");
    }
    const auto size = static_cast<std::uint64_t>(signed_size.QuadPart);
    if (size == 0) {
        return Failure(ByteLoadError::empty_file, ERROR_HANDLE_EOF, L"empty assets are not valid overrides");
    }
    if (size > max_file_size || size > (std::numeric_limits<std::size_t>::max)()) {
        return Failure(ByteLoadError::file_too_large, ERROR_FILE_TOO_LARGE, L"asset exceeds the configured size limit");
    }

    std::vector<std::byte> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        return Failure(ByteLoadError::allocation_failed, ERROR_NOT_ENOUGH_MEMORY, L"asset buffer allocation failed");
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD bytes_read = 0;
        if (!ReadFile(file.get(), bytes.data() + offset, chunk, &bytes_read, nullptr)) {
            return Failure(ByteLoadError::read_failed, GetLastError(), L"asset read failed");
        }
        if (bytes_read == 0) {
            return Failure(ByteLoadError::read_failed, ERROR_HANDLE_EOF, L"asset was truncated while reading");
        }
        offset += bytes_read;
    }

    const auto digest = ComputeSha256(bytes);
    if (!digest) {
        return Failure(ByteLoadError::hash_failed, digest.win32_error, L"SHA-256 calculation failed");
    }

    ByteLoadResult result;
    try {
        result.storage = std::shared_ptr<const ByteStorage>(
            new ByteStorage(std::move(bytes), digest.digest));
    } catch (const std::bad_alloc&) {
        return Failure(ByteLoadError::allocation_failed, ERROR_NOT_ENOUGH_MEMORY, L"asset object allocation failed");
    }
    return result;
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
