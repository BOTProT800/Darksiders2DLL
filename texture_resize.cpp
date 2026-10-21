#include "texture_resize.h"
#include "native_texture.h"
#include <algorithm>
#include <limits>

namespace ds2::modding {
namespace {
// The extra stores each side in a u16.
constexpr std::uint32_t kMaxSide = 0xFFFF;
constexpr std::size_t kHeaderSize = 128;

std::uint64_t LevelBytes(const DdsFormat format, const std::uint32_t width, const std::uint32_t height) noexcept {
    const auto blocks = [](const std::uint32_t side) { return (static_cast<std::uint64_t>(side) + 3) / 4; };
    switch (format) {
    case DdsFormat::bc1: return blocks(width) * blocks(height) * 8;
    case DdsFormat::bc2:
    case DdsFormat::bc3: return blocks(width) * blocks(height) * 16;
    case DdsFormat::bgra8: return static_cast<std::uint64_t>(width) * height * 4;
    default: return 0;
    }
}

std::vector<std::uint32_t> Runs(const std::vector<std::uint32_t>& levels) {
    std::vector<std::uint32_t> runs;
    std::uint32_t total = 0;
    for (const auto level : levels) runs.push_back(total += level);
    return runs;
}
}  // namespace

std::optional<TextureEngineExtra> EncodeTextureEngineExtra(const DdsMetadata& dds) noexcept {
    const auto format = NativeTextureFormat(dds.format);
    if (format == kUnsupportedNativeTextureFormat || dds.has_dx10_header || dds.header_size != kHeaderSize ||
        dds.width == 0 || dds.height == 0 || dds.width > kMaxSide || dds.height > kMaxSide ||
        dds.mip_count == 0 || dds.mip_count > 255) return std::nullopt;
    return TextureEngineExtra{std::byte(format), std::byte(dds.mip_count), std::byte{1},
        std::byte(dds.width & 0xFF), std::byte(dds.width >> 8), std::byte(dds.height & 0xFF), std::byte(dds.height >> 8)};
}

std::vector<std::uint32_t> TextureEngineLevels(const DdsMetadata& dds) {
    std::vector<std::uint32_t> levels;
    std::uint32_t width = dds.width, height = dds.height;
    for (std::uint32_t level = 0; level < dds.mip_count; ++level) {
        const auto bytes = LevelBytes(dds.format, width, height);
        if (bytes == 0 || bytes > (std::numeric_limits<std::int32_t>::max)()) return {};
        levels.push_back(static_cast<std::uint32_t>(bytes));
        width = (std::max)(1u, width / 2);
        height = (std::max)(1u, height / 2);
    }
    return levels;
}

std::uint32_t TextureEngineMipSkip(const std::uint32_t mips, std::uint32_t width, std::uint32_t height,
                                   const std::uint16_t limit, const std::int32_t detail) noexcept {
    std::uint32_t skip = 0;
    if (limit != 0 && limit < mips) {
        skip = mips - limit;
    } else if (detail != 2) {
        // The handler keeps 2 - detail in 16 bits and compares it signed.
        const std::uint32_t drop = static_cast<std::uint16_t>(2 - detail);
        if (drop < mips && mips - drop >= 7) skip = drop;
    }
    for (std::uint32_t level = 0; level < skip; ++level) {
        width = (std::max)(1u, width / 2);
        height = (std::max)(1u, height / 2);
    }
    return width < 4 || height < 4 ? 0 : skip;
}

std::optional<TextureResize> PlanTextureResize(
    const PackageIdentityEntry& entry, const DdsMetadata& original,
    std::shared_ptr<const ByteStorage> storage, const DdsMetadata& replacement, std::wstring& error) {
    const auto packaged = EncodeTextureEngineExtra(original);
    if (!packaged || entry.metadata_size < kTextureEngineExtraSize || entry.extra.size() != kTextureEngineExtraSize ||
        entry.extra_offset > entry.metadata_size - kTextureEngineExtraSize ||
        !std::equal(entry.extra.begin(), entry.extra.end(), packaged->begin())) {
        error = L"package metadata extra does not repeat the original DDS contract";
        return std::nullopt;
    }
    const auto extra = EncodeTextureEngineExtra(replacement);
    if (!extra) {
        error = L"the DLL knows engine codes only for BGRA8, BC1, BC2 and BC3 2D textures, up to 65535 per side";
        return std::nullopt;
    }
    if (*extra == *packaged) {
        error = L"replacement has the original contract";
        return std::nullopt;
    }
    TextureResize plan{entry.metadata_size, entry.extra_offset, entry.metadata_sha256, *extra,
        std::move(storage), replacement, {}, {}, {}};
    // The handler (0x75A63C) does not check that CreateTexture worked. These
    // are D3D9 rules from its documentation, not failures seen in game.
    if (replacement.format != DdsFormat::bgra8 && (replacement.width % 4 != 0 || replacement.height % 4 != 0))
        plan.warnings.push_back(L"untested: D3D9 documents BC sides as multiples of 4");
    if (replacement.width > 16384 || replacement.height > 16384)
        plan.warnings.push_back(L"untested: sides over 16384 may exceed the device limit");
    const auto levels = TextureEngineLevels(replacement), original_levels = TextureEngineLevels(original);
    plan.replacement_runs = Runs(levels);
    plan.original_runs = Runs(original_levels);
    // The engine reads 128 + the sum of its levels: nothing more, nothing less.
    if (!plan.storage || levels.empty() || original_levels.empty() ||
        plan.storage->Size() != kHeaderSize + static_cast<std::uint64_t>(plan.replacement_runs.back()) ||
        replacement.payload_size != plan.replacement_runs.back() ||
        original.payload_size != plan.original_runs.back() ||
        entry.original_size != kHeaderSize + original.payload_size) {
        error = L"DDS sizes differ from the engine's level layout";
        return std::nullopt;
    }
    return plan;
}
}
