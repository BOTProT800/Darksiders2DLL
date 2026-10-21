#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ds2::modding {

enum class DdsFormat {
    unknown,
    bc1,
    bc2,
    bc3,
    bc4,
    bc5,
    bgr8,
    bgra8,
    rgba8,
};

enum class DdsValidationError {
    none,
    too_small,
    bad_magic,
    bad_header,
    invalid_dimensions,
    invalid_depth,
    unsupported_surface,
    unsupported_format,
    invalid_mip_count,
    arithmetic_overflow,
    size_mismatch,
    expectation_mismatch,
};

struct DdsMetadata final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t mip_count{};
    DdsFormat format{DdsFormat::unknown};
    std::size_t header_size{};
    std::uint64_t payload_size{};
    std::uint64_t expected_file_size{};
    bool has_dx10_header{};
};

struct DdsExpectations final {
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::uint32_t> mip_count;
    std::optional<DdsFormat> format;
    std::optional<std::uint64_t> exact_file_size;
    // The engine's texture extra stores each side in a u16.
    std::uint32_t max_dimension{0xFFFF};
};

struct DdsValidationResult final {
    std::optional<DdsMetadata> metadata;
    DdsValidationError error{DdsValidationError::none};
    std::wstring detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return metadata.has_value();
    }
};

[[nodiscard]] DdsValidationResult ValidateDds(
    std::span<const std::byte> bytes,
    const DdsExpectations& expectations = {});

// The same texture behind a 128-byte legacy header: a DX10 header becomes the
// FourCC or RGB pixel format of its format (sRGB is dropped, the bits stay).
// The engine skips 128 header bytes, so DX10 files go through this first.
[[nodiscard]] std::optional<std::vector<std::byte>> LegacyDdsBytes(
    std::span<const std::byte> bytes, const DdsMetadata& metadata);

[[nodiscard]] std::wstring_view DdsFormatName(DdsFormat format) noexcept;
[[nodiscard]] std::wstring_view DdsValidationErrorName(DdsValidationError error) noexcept;

}  // namespace ds2::modding
