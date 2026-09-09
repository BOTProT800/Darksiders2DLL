#include "sha256.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ds2::modding {
namespace {

class AlgorithmHandle final {
public:
    ~AlgorithmHandle() {
        if (value != nullptr) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }

    BCRYPT_ALG_HANDLE value{};
};

class HashHandle final {
public:
    ~HashHandle() {
        if (value != nullptr) {
            BCryptDestroyHash(value);
        }
    }

    BCRYPT_HASH_HANDLE value{};
};

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

class Sha256Hasher final {
public:
    [[nodiscard]] bool Initialize() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm_.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return false;
        }

        ULONG copied = 0;
        ULONG object_length = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                algorithm_.value,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_length),
                sizeof(object_length),
                &copied,
                0)) ||
            copied != sizeof(object_length)) {
            return false;
        }

        ULONG digest_length = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                algorithm_.value,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_length),
                sizeof(digest_length),
                &copied,
                0)) ||
            copied != sizeof(digest_length) || digest_length != Sha256Digest{}.size()) {
            return false;
        }

        object_.resize(object_length);
        return BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm_.value,
            &hash_.value,
            object_.data(),
            static_cast<ULONG>(object_.size()),
            nullptr,
            0,
            0));
    }

    [[nodiscard]] bool Update(const std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const ULONG chunk = static_cast<ULONG>((std::min)(
                remaining,
                static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
            if (!BCRYPT_SUCCESS(BCryptHashData(
                    hash_.value,
                    reinterpret_cast<PUCHAR>(
                        const_cast<std::byte*>(bytes.data() + offset)),
                    chunk,
                    0))) {
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    [[nodiscard]] bool Finish(Sha256Digest& output) {
        return BCRYPT_SUCCESS(BCryptFinishHash(
            hash_.value,
            output.data(),
            static_cast<ULONG>(output.size()),
            0));
    }

private:
    AlgorithmHandle algorithm_;
    HashHandle hash_;
    std::vector<UCHAR> object_;
};

int HexNibble(const wchar_t value) noexcept {
    if (value >= L'0' && value <= L'9') {
        return value - L'0';
    }
    if (value >= L'a' && value <= L'f') {
        return value - L'a' + 10;
    }
    if (value >= L'A' && value <= L'F') {
        return value - L'A' + 10;
    }
    return -1;
}

}  // namespace

Sha256Result ComputeSha256(const std::span<const std::byte> bytes) {
    Sha256Result result;
    result.file_size = bytes.size();
    Sha256Hasher hasher;
    if (!hasher.Initialize() || !hasher.Update(bytes) || !hasher.Finish(result.digest)) {
        result.error = Sha256Error::crypto_failed;
    }
    return result;
}

Sha256Result ComputeFileSha256(
    const std::filesystem::path& path,
    const std::uint64_t max_file_size) {
    Sha256Result result;
    if (path.empty()) {
        result.error = Sha256Error::invalid_argument;
        result.win32_error = ERROR_INVALID_PARAMETER;
        return result;
    }

    const FileHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        result.error = Sha256Error::open_failed;
        result.win32_error = GetLastError();
        return result;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) {
        result.error = Sha256Error::metadata_failed;
        result.win32_error = GetLastError();
        return result;
    }
    result.file_size = static_cast<std::uint64_t>(size.QuadPart);
    if (result.file_size > max_file_size) {
        result.error = Sha256Error::file_too_large;
        result.win32_error = ERROR_FILE_TOO_LARGE;
        return result;
    }

    Sha256Hasher hasher;
    if (!hasher.Initialize()) {
        result.error = Sha256Error::crypto_failed;
        return result;
    }

    std::vector<std::byte> buffer(1024 * 1024);
    std::uint64_t total_read = 0;
    while (true) {
        DWORD bytes_read = 0;
        if (!ReadFile(
                file.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_read,
                nullptr)) {
            result.error = Sha256Error::read_failed;
            result.win32_error = GetLastError();
            return result;
        }
        if (bytes_read == 0) {
            break;
        }
        total_read += bytes_read;
        if (!hasher.Update(std::span<const std::byte>(buffer.data(), bytes_read))) {
            result.error = Sha256Error::crypto_failed;
            return result;
        }
    }

    if (total_read != result.file_size) {
        result.error = Sha256Error::read_failed;
        result.win32_error = ERROR_HANDLE_EOF;
        return result;
    }
    if (!hasher.Finish(result.digest)) {
        result.error = Sha256Error::crypto_failed;
        return result;
    }
    return result;
}

std::string Sha256Hex(const Sha256Digest& digest) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        output[index * 2] = kHex[digest[index] >> 4];
        output[index * 2 + 1] = kHex[digest[index] & 0x0F];
    }
    return output;
}

std::wstring Sha256HexWide(const Sha256Digest& digest) {
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring output;
    output.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        output[index * 2] = kHex[digest[index] >> 4];
        output[index * 2 + 1] = kHex[digest[index] & 0x0F];
    }
    return output;
}

bool Sha256EqualsHex(
    const Sha256Digest& digest,
    const std::wstring_view expected_hex) noexcept {
    if (expected_hex.size() != digest.size() * 2) {
        return false;
    }
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const int high = HexNibble(expected_hex[index * 2]);
        const int low = HexNibble(expected_hex[index * 2 + 1]);
        if (high < 0 || low < 0 ||
            digest[index] != static_cast<std::uint8_t>((high << 4) | low)) {
            return false;
        }
    }
    return true;
}

}  // namespace ds2::modding
