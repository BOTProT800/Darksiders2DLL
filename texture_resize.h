#pragma once
#include "byte_storage.h"
#include "dds_validation.h"
#include "package_identity_catalog.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ds2::modding {
// A DDS member's package extra (research/TEXTURE_RESIZE_RESEARCH.md): u8 engine
// format, u8 mips, u8 kind (1 = 2D), u16 width, u16 height. The texture
// handlers (D3D9 0x75A63C, DX11 0x1087B98) size the texture from it alone,
// skip the member's 128-byte header and read the payload into the texture.
inline constexpr std::size_t kTextureEngineExtraSize = 7;
using TextureEngineExtra = std::array<std::byte, kTextureEngineExtraSize>;

// Empty for anything the engine cannot take: formats other than BGRA8 and
// BC1-BC3, DX10 headers, sides over 16384 or more than 255 mips.
[[nodiscard]] std::optional<TextureEngineExtra> EncodeTextureEngineExtra(const DdsMetadata& dds) noexcept;
// Byte size of each mip level, as 0x7FC560 computes them.
[[nodiscard]] std::vector<std::uint32_t> TextureEngineLevels(const DdsMetadata& dds);
// Top levels the D3D9 handler drops (0x75A63C). [Resources] MaxMipLevels
// (`limit`, 0 = off) comes first; otherwise [Renderer] TextureDetail below 2
// drops 2 - detail levels when at least 7 remain. No drop when the first kept
// level is under 4 pixels on a side. DX11 only applies the limit.
[[nodiscard]] std::uint32_t TextureEngineMipSkip(std::uint32_t mips, std::uint32_t width, std::uint32_t height,
                                                 std::uint16_t limit, std::int32_t detail) noexcept;

// A replacement of another size, format or mip count: its extra is written
// over this member's entry in the package metadata block when the engine reads
// it, and the member's payload is then served from the replacement after the
// original has been consumed from the stream.
struct TextureResize final {
    std::uint32_t metadata_size{}, extra_offset{};
    Sha256Digest metadata_sha256{};
    TextureEngineExtra extra{};
    std::shared_ptr<const ByteStorage> storage;  // the replacement as is
    DdsMetadata dds;
    // The engine's first payload read is a run of whole levels from the top,
    // as long as the texture's memory is contiguous: one size per prefix.
    std::vector<std::uint32_t> replacement_runs, original_runs;
};
// Requires the package extra to repeat the original contract (schema check).
[[nodiscard]] std::optional<TextureResize> PlanTextureResize(
    const PackageIdentityEntry& entry, const DdsMetadata& original,
    std::shared_ptr<const ByteStorage> storage, const DdsMetadata& replacement, std::wstring& error);
}
