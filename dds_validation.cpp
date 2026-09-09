#include "dds_validation.h"

#include <algorithm>
#include <limits>

namespace ds2::modding {
namespace {

constexpr std::uint32_t MakeFourCc(
    const char first,
    const char second,
    const char third,
    const char fourth) noexcept {
    return static_cast<std::uint8_t>(first) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(second)) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(third)) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(fourth)) << 24);
}

constexpr std::uint32_t kDdsMagic = MakeFourCc('D', 'D', 'S', ' ');
constexpr std::uint32_t kFourCcDxt1 = MakeFourCc('D', 'X', 'T', '1');
constexpr std::uint32_t kFourCcDxt3 = MakeFourCc('D', 'X', 'T', '3');
constexpr std::uint32_t kFourCcDxt5 = MakeFourCc('D', 'X', 'T', '5');
constexpr std::uint32_t kFourCcAti1 = MakeFourCc('A', 'T', 'I', '1');
constexpr std::uint32_t kFourCcAti2 = MakeFourCc('A', 'T', 'I', '2');
constexpr std::uint32_t kFourCcBc4U = MakeFourCc('B', 'C', '4', 'U');
constexpr std::uint32_t kFourCcBc5U = MakeFourCc('B', 'C', '5', 'U');
constexpr std::uint32_t kFourCcDx10 = MakeFourCc('D', 'X', '1', '0');

constexpr std::uint32_t kDdsdHeight = 0x00000002;
constexpr std::uint32_t kDdsdWidth = 0x00000004;
constexpr std::uint32_t kDdsdPixelFormat = 0x00001000;
constexpr std::uint32_t kDdsdMipMapCount = 0x00020000;
constexpr std::uint32_t kDdsdDepth = 0x00800000;
constexpr std::uint32_t kDdpfFourCc = 0x00000004;
constexpr std::uint32_t kDdpfRgb = 0x00000040;
constexpr std::uint32_t kCaps2CubeMap = 0x00000200;
constexpr std::uint32_t kCaps2Volume = 0x00200000;
constexpr std::uint32_t kDx10Texture2d = 3;
constexpr std::uint32_t kDx10MiscTextureCube = 0x4;

std::uint32_t ReadU32(const std::span<const std::byte> bytes, const std::size_t offset) {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24);
}

DdsValidationResult Failure(
    const DdsValidationError error,
    std::wstring detail) {
    DdsValidationResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

struct FormatLayout final {
    DdsFormat format{DdsFormat::unknown};
    std::uint32_t block_bytes{};
    std::uint32_t bytes_per_pixel{};
};

FormatLayout LegacyFormat(
    const std::uint32_t pixel_flags,
    const std::uint32_t four_cc,
    const std::uint32_t rgb_bits,
    const std::uint32_t red_mask,
    const std::uint32_t green_mask,
    const std::uint32_t blue_mask,
    const std::uint32_t alpha_mask) noexcept {
    if ((pixel_flags & kDdpfFourCc) != 0) {
        switch (four_cc) {
        case kFourCcDxt1:
            return {DdsFormat::bc1, 8, 0};
        case kFourCcDxt3:
            return {DdsFormat::bc2, 16, 0};
        case kFourCcDxt5:
            return {DdsFormat::bc3, 16, 0};
        case kFourCcAti1:
        case kFourCcBc4U:
            return {DdsFormat::bc4, 8, 0};
        case kFourCcAti2:
        case kFourCcBc5U:
            return {DdsFormat::bc5, 16, 0};
        default:
            return {};
        }
    }

    if ((pixel_flags & kDdpfRgb) == 0) {
        return {};
    }
    if (rgb_bits == 24 && red_mask == 0x00FF0000 && green_mask == 0x0000FF00 &&
        blue_mask == 0x000000FF && alpha_mask == 0) {
        return {DdsFormat::bgr8, 0, 3};
    }
    if (rgb_bits == 32 && red_mask == 0x00FF0000 && green_mask == 0x0000FF00 &&
        blue_mask == 0x000000FF && alpha_mask == 0xFF000000) {
        return {DdsFormat::bgra8, 0, 4};
    }
    if (rgb_bits == 32 && red_mask == 0x000000FF && green_mask == 0x0000FF00 &&
        blue_mask == 0x00FF0000 && alpha_mask == 0xFF000000) {
        return {DdsFormat::rgba8, 0, 4};
    }
    return {};
}

FormatLayout Dx10Format(const std::uint32_t dxgi_format) noexcept {
    switch (dxgi_format) {
    case 28:  // DXGI_FORMAT_R8G8B8A8_UNORM
    case 29:  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        return {DdsFormat::rgba8, 0, 4};
    case 71:
    case 72:
        return {DdsFormat::bc1, 8, 0};
    case 74:
    case 75:
        return {DdsFormat::bc2, 16, 0};
    case 77:
    case 78:
        return {DdsFormat::bc3, 16, 0};
    case 80:
    case 81:
        return {DdsFormat::bc4, 8, 0};
    case 83:
    case 84:
        return {DdsFormat::bc5, 16, 0};
    case 87:  // DXGI_FORMAT_B8G8R8A8_UNORM
    case 91:  // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        return {DdsFormat::bgra8, 0, 4};
    default:
        return {};
    }
}

bool CheckedAdd(const std::uint64_t left, const std::uint64_t right, std::uint64_t& out) {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    out = left + right;
    return true;
}

bool LevelSize(
    const std::uint32_t width,
    const std::uint32_t height,
    const FormatLayout layout,
    std::uint64_t& output) {
    if (layout.block_bytes != 0) {
        const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
        const std::uint64_t blocks_high = (static_cast<std::uint64_t>(height) + 3) / 4;
        if (blocks_wide > (std::numeric_limits<std::uint64_t>::max)() / blocks_high ||
            blocks_wide * blocks_high >
                (std::numeric_limits<std::uint64_t>::max)() / layout.block_bytes) {
            return false;
        }
        output = blocks_wide * blocks_high * layout.block_bytes;
        return true;
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (layout.bytes_per_pixel == 0 ||
        pixels > (std::numeric_limits<std::uint64_t>::max)() / layout.bytes_per_pixel) {
        return false;
    }
    output = pixels * layout.bytes_per_pixel;
    return true;
}

std::uint32_t MaximumMipCount(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width = (std::max)(1u, width / 2);
        height = (std::max)(1u, height / 2);
        ++levels;
    }
    return levels;
}

}  // namespace

DdsValidationResult ValidateDds(
    const std::span<const std::byte> bytes,
    const DdsExpectations& expectations) {
    constexpr std::size_t kLegacyHeaderSize = 128;
    constexpr std::size_t kDx10HeaderSize = 148;
    if (bytes.size() < kLegacyHeaderSize) {
        return Failure(DdsValidationError::too_small, L"DDS header is truncated");
    }
    if (ReadU32(bytes, 0) != kDdsMagic) {
        return Failure(DdsValidationError::bad_magic, L"missing DDS magic");
    }
    if (ReadU32(bytes, 4) != 124 || ReadU32(bytes, 76) != 32) {
        return Failure(DdsValidationError::bad_header, L"invalid DDS/DDS_PIXELFORMAT size");
    }

    const std::uint32_t flags = ReadU32(bytes, 8);
    const std::uint32_t height = ReadU32(bytes, 12);
    const std::uint32_t width = ReadU32(bytes, 16);
    const std::uint32_t depth = ReadU32(bytes, 24);
    const std::uint32_t raw_mip_count = ReadU32(bytes, 28);
    const std::uint32_t pixel_flags = ReadU32(bytes, 80);
    const std::uint32_t four_cc = ReadU32(bytes, 84);
    const std::uint32_t caps2 = ReadU32(bytes, 112);

    if ((flags & (kDdsdWidth | kDdsdHeight | kDdsdPixelFormat)) !=
        (kDdsdWidth | kDdsdHeight | kDdsdPixelFormat)) {
        return Failure(DdsValidationError::bad_header, L"required DDS flags are absent");
    }
    if (width == 0 || height == 0 || width > expectations.max_dimension ||
        height > expectations.max_dimension) {
        return Failure(DdsValidationError::invalid_dimensions, L"invalid or excessive dimensions");
    }
    if ((flags & kDdsdDepth) != 0 || depth > 1 ||
        (caps2 & (kCaps2CubeMap | kCaps2Volume)) != 0) {
        return Failure(DdsValidationError::unsupported_surface, L"only a single 2D surface is accepted");
    }

    const std::uint32_t mip_count = raw_mip_count == 0 ? 1 : raw_mip_count;
    if (((flags & kDdsdMipMapCount) == 0 && raw_mip_count > 1) ||
        mip_count > MaximumMipCount(width, height)) {
        return Failure(DdsValidationError::invalid_mip_count, L"invalid mip count");
    }

    bool has_dx10_header = false;
    std::size_t header_size = kLegacyHeaderSize;
    FormatLayout layout;
    if ((pixel_flags & kDdpfFourCc) != 0 && four_cc == kFourCcDx10) {
        if (bytes.size() < kDx10HeaderSize) {
            return Failure(DdsValidationError::too_small, L"DDS DX10 header is truncated");
        }
        has_dx10_header = true;
        header_size = kDx10HeaderSize;
        const std::uint32_t dxgi_format = ReadU32(bytes, 128);
        const std::uint32_t resource_dimension = ReadU32(bytes, 132);
        const std::uint32_t misc_flag = ReadU32(bytes, 136);
        const std::uint32_t array_size = ReadU32(bytes, 140);
        if (resource_dimension != kDx10Texture2d || array_size != 1 ||
            (misc_flag & kDx10MiscTextureCube) != 0) {
            return Failure(DdsValidationError::unsupported_surface, L"unsupported DX10 resource shape");
        }
        layout = Dx10Format(dxgi_format);
    } else {
        layout = LegacyFormat(
            pixel_flags,
            four_cc,
            ReadU32(bytes, 88),
            ReadU32(bytes, 92),
            ReadU32(bytes, 96),
            ReadU32(bytes, 100),
            ReadU32(bytes, 104));
    }
    if (layout.format == DdsFormat::unknown) {
        return Failure(DdsValidationError::unsupported_format, L"DDS pixel format is not in the safe allow-list");
    }

    std::uint64_t payload_size = 0;
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    for (std::uint32_t level = 0; level < mip_count; ++level) {
        std::uint64_t level_size = 0;
        if (!LevelSize(level_width, level_height, layout, level_size) ||
            !CheckedAdd(payload_size, level_size, payload_size)) {
            return Failure(DdsValidationError::arithmetic_overflow, L"DDS payload size overflows");
        }
        level_width = (std::max)(1u, level_width / 2);
        level_height = (std::max)(1u, level_height / 2);
    }

    std::uint64_t expected_file_size = 0;
    if (!CheckedAdd(header_size, payload_size, expected_file_size)) {
        return Failure(DdsValidationError::arithmetic_overflow, L"DDS file size overflows");
    }
    if (bytes.size() != expected_file_size) {
        return Failure(DdsValidationError::size_mismatch, L"DDS byte count does not match dimensions, format and mip count");
    }

    if ((expectations.width.has_value() && *expectations.width != width) ||
        (expectations.height.has_value() && *expectations.height != height) ||
        (expectations.mip_count.has_value() && *expectations.mip_count != mip_count) ||
        (expectations.format.has_value() && *expectations.format != layout.format) ||
        (expectations.exact_file_size.has_value() &&
         *expectations.exact_file_size != expected_file_size)) {
        return Failure(DdsValidationError::expectation_mismatch, L"DDS does not match the requested asset contract");
    }

    DdsValidationResult result;
    result.metadata = DdsMetadata{
        width,
        height,
        mip_count,
        layout.format,
        header_size,
        payload_size,
        expected_file_size,
        has_dx10_header};
    return result;
}

std::wstring_view DdsFormatName(const DdsFormat format) noexcept {
    switch (format) {
    case DdsFormat::unknown:
        return L"UNKNOWN";
    case DdsFormat::bc1:
        return L"BC1/DXT1";
    case DdsFormat::bc2:
        return L"BC2/DXT3";
    case DdsFormat::bc3:
        return L"BC3/DXT5";
    case DdsFormat::bc4:
        return L"BC4";
    case DdsFormat::bc5:
        return L"BC5";
    case DdsFormat::bgr8:
        return L"BGR8";
    case DdsFormat::bgra8:
        return L"BGRA8";
    case DdsFormat::rgba8:
        return L"RGBA8";
    }
    return L"UNKNOWN";
}

std::wstring_view DdsValidationErrorName(const DdsValidationError error) noexcept {
    switch (error) {
    case DdsValidationError::none:
        return L"none";
    case DdsValidationError::too_small:
        return L"too_small";
    case DdsValidationError::bad_magic:
        return L"bad_magic";
    case DdsValidationError::bad_header:
        return L"bad_header";
    case DdsValidationError::invalid_dimensions:
        return L"invalid_dimensions";
    case DdsValidationError::invalid_depth:
        return L"invalid_depth";
    case DdsValidationError::unsupported_surface:
        return L"unsupported_surface";
    case DdsValidationError::unsupported_format:
        return L"unsupported_format";
    case DdsValidationError::invalid_mip_count:
        return L"invalid_mip_count";
    case DdsValidationError::arithmetic_overflow:
        return L"arithmetic_overflow";
    case DdsValidationError::size_mismatch:
        return L"size_mismatch";
    case DdsValidationError::expectation_mismatch:
        return L"expectation_mismatch";
    }
    return L"unknown";
}

}  // namespace ds2::modding
