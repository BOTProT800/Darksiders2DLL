#pragma once

#include "sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ds2::modding {

enum class ByteLoadError {
    none,
    invalid_path,
    open_failed,
    reparse_point,
    not_regular_file,
    metadata_failed,
    empty_file,
    file_too_large,
    allocation_failed,
    read_failed,
    hash_failed,
};

class ByteStorage final {
public:
    ByteStorage(const ByteStorage&) = delete;
    ByteStorage& operator=(const ByteStorage&) = delete;

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] const std::byte* Data() const noexcept { return bytes_.data(); }
    [[nodiscard]] std::size_t Size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const Sha256Digest& Sha256() const noexcept { return sha256_; }
    [[nodiscard]] const std::wstring& Sha256Hex() const noexcept { return sha256_hex_; }

private:
    friend struct ByteLoadResult;
    friend ByteLoadResult LoadByteStorage(const std::filesystem::path&, std::uint64_t);

    ByteStorage(std::vector<std::byte> bytes, Sha256Digest digest);

    std::vector<std::byte> bytes_;
    Sha256Digest sha256_{};
    std::wstring sha256_hex_;
};

struct ByteLoadResult final {
    std::shared_ptr<const ByteStorage> storage;
    ByteLoadError error{ByteLoadError::none};
    unsigned long win32_error{};
    std::wstring detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return storage != nullptr;
    }
};

// Opens the final path component itself and rejects all reparse-point files.
// The handle denies concurrent writers so the snapshot cannot contain a torn
// read. Empty files are rejected because they cannot be valid asset overrides.
[[nodiscard]] ByteLoadResult LoadByteStorage(
    const std::filesystem::path& path,
    std::uint64_t max_file_size = 128ull * 1024ull * 1024ull);

[[nodiscard]] std::wstring_view ByteLoadErrorName(ByteLoadError error) noexcept;

}  // namespace ds2::modding
