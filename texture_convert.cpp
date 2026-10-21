#include "texture_convert.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ds2::modding {
namespace {
using Texel = std::array<std::uint8_t, 4>; // RGBA
struct Image final {
    std::uint32_t width{}, height{};
    std::vector<Texel> texels;
    const Texel& At(const std::uint32_t x, const std::uint32_t y) const {
        return texels[static_cast<std::size_t>((std::min)(y, height - 1)) * width + (std::min)(x, width - 1)];
    }
};
constexpr std::uint64_t kMaxPixels = 4096ull * 4096ull;

std::uint8_t ByteAt(const std::span<const std::byte> bytes, const std::size_t offset) {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}
std::uint32_t BlockBytes(const DdsFormat format) noexcept {
    return format == DdsFormat::bc1 ? 8 : format == DdsFormat::bc2 || format == DdsFormat::bc3 ? 16 : 0;
}
std::uint32_t PixelBytes(const DdsFormat format) noexcept {
    return format == DdsFormat::bgr8 ? 3 : format == DdsFormat::bgra8 || format == DdsFormat::rgba8 ? 4 : 0;
}
std::uint64_t LevelBytes(const DdsFormat format, const std::uint32_t width, const std::uint32_t height) {
    if (const auto block = BlockBytes(format))
        return ((width + 3ull) / 4) * ((height + 3ull) / 4) * block;
    return static_cast<std::uint64_t>(width) * height * PixelBytes(format);
}
Texel Color565(const std::uint16_t color) {
    const auto r = (color >> 11) & 31, g = (color >> 5) & 63, b = color & 31;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}
Texel Mix(const Texel& a, const Texel& b, const int wa, const int wb) {
    Texel out{};
    for (std::size_t c = 0; c < 3; ++c)
        out[c] = static_cast<std::uint8_t>((a[c] * wa + b[c] * wb) / (wa + wb));
    out[3] = 255;
    return out;
}
// The palette a GPU derives from two endpoints; BC2/BC3 always use 4 colors.
std::array<Texel, 4> ColorPalette(const std::uint16_t c0, const std::uint16_t c1, const bool four_colors) {
    const auto a = Color565(c0), b = Color565(c1);
    if (four_colors || c0 > c1) return {a, b, Mix(a, b, 2, 1), Mix(a, b, 1, 2)};
    return {a, b, Mix(a, b, 1, 1), Texel{0, 0, 0, 0}};
}
std::array<std::uint8_t, 8> AlphaPalette(const std::uint8_t a0, const std::uint8_t a1) {
    std::array<std::uint8_t, 8> p{a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) p[i + 1] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i < 5; ++i) p[i + 1] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
        p[6] = 0;
        p[7] = 255;
    }
    return p;
}

Image Decode(const std::span<const std::byte> bytes, const DdsMetadata& dds) {
    Image image{dds.width, dds.height, std::vector<Texel>(static_cast<std::size_t>(dds.width) * dds.height)};
    const auto payload = bytes.subspan(dds.header_size);
    if (payload.size() < LevelBytes(dds.format, dds.width, dds.height)) throw std::runtime_error("truncated level");
    if (const auto block_bytes = BlockBytes(dds.format)) {
        const std::uint32_t blocks_wide = (dds.width + 3) / 4, blocks_high = (dds.height + 3) / 4;
        for (std::uint32_t by = 0; by < blocks_high; ++by) {
            for (std::uint32_t bx = 0; bx < blocks_wide; ++bx) {
                const std::size_t at = (static_cast<std::size_t>(by) * blocks_wide + bx) * block_bytes;
                const std::size_t color = dds.format == DdsFormat::bc1 ? at : at + 8;
                const auto c0 = static_cast<std::uint16_t>(ByteAt(payload, color) | ByteAt(payload, color + 1) << 8);
                const auto c1 = static_cast<std::uint16_t>(ByteAt(payload, color + 2) | ByteAt(payload, color + 3) << 8);
                const auto palette = ColorPalette(c0, c1, dds.format != DdsFormat::bc1);
                std::uint64_t alpha_bits = 0;
                for (std::size_t i = 0; i < 6; ++i) alpha_bits |= static_cast<std::uint64_t>(ByteAt(payload, at + 2 + i)) << (8 * i);
                const auto alphas = AlphaPalette(ByteAt(payload, at), ByteAt(payload, at + 1));
                for (std::uint32_t i = 0; i < 16; ++i) {
                    const std::uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x >= dds.width || y >= dds.height) continue;
                    Texel texel = palette[(ByteAt(payload, color + 4 + i / 4) >> (2 * (i % 4))) & 3];
                    if (dds.format == DdsFormat::bc2)
                        texel[3] = static_cast<std::uint8_t>(((ByteAt(payload, at + i / 2) >> (4 * (i % 2))) & 15) * 17);
                    else if (dds.format == DdsFormat::bc3)
                        texel[3] = alphas[(alpha_bits >> (3 * i)) & 7];
                    image.texels[static_cast<std::size_t>(y) * dds.width + x] = texel;
                }
            }
        }
        return image;
    }
    const auto stride = PixelBytes(dds.format);
    for (std::size_t i = 0; i < image.texels.size(); ++i) {
        const std::size_t at = i * stride;
        const std::uint8_t c0 = ByteAt(payload, at), c1 = ByteAt(payload, at + 1), c2 = ByteAt(payload, at + 2);
        const std::uint8_t a = stride == 4 ? ByteAt(payload, at + 3) : 255;
        image.texels[i] = dds.format == DdsFormat::rgba8 ? Texel{c0, c1, c2, a} : Texel{c2, c1, c0, a};
    }
    return image;
}

// Separable triangle filter: bilinear when enlarging, widened to average
// every covered texel when shrinking.
std::vector<std::vector<std::pair<std::uint32_t, float>>> Taps(const std::uint32_t from, const std::uint32_t to) {
    std::vector<std::vector<std::pair<std::uint32_t, float>>> taps(to);
    const double scale = static_cast<double>(from) / to, support = (std::max)(1.0, scale);
    for (std::uint32_t i = 0; i < to; ++i) {
        const double center = (i + 0.5) * scale - 0.5;
        double total = 0;
        for (auto j = static_cast<std::int64_t>(std::ceil(center - support));
             j <= static_cast<std::int64_t>(std::floor(center + support)); ++j) {
            const double weight = 1.0 - std::abs(static_cast<double>(j) - center) / support;
            if (weight <= 0) continue;
            taps[i].emplace_back(static_cast<std::uint32_t>(std::clamp<std::int64_t>(j, 0, from - 1)),
                                 static_cast<float>(weight));
            total += weight;
        }
        if (taps[i].empty()) {
            taps[i].emplace_back(static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                std::llround(center), 0, from - 1)), 1.0f);
            total = 1;
        }
        for (auto& tap : taps[i]) tap.second = static_cast<float>(tap.second / total);
    }
    return taps;
}
Image Resize(const Image& source, const std::uint32_t width, const std::uint32_t height) {
    if (source.width == width && source.height == height) return source;
    const auto columns = Taps(source.width, width), rows = Taps(source.height, height);
    const auto blend = [](const auto& taps, const auto& fetch) {
        std::array<float, 4> sum{};
        for (const auto& [index, weight] : taps) {
            const Texel& texel = fetch(index);
            for (std::size_t c = 0; c < 4; ++c) sum[c] += texel[c] * weight;
        }
        Texel out{};
        for (std::size_t c = 0; c < 4; ++c)
            out[c] = static_cast<std::uint8_t>(std::clamp(std::lround(sum[c]), 0l, 255l));
        return out;
    };
    Image wide{width, source.height, std::vector<Texel>(static_cast<std::size_t>(width) * source.height)};
    for (std::uint32_t y = 0; y < source.height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            wide.texels[static_cast<std::size_t>(y) * width + x] =
                blend(columns[x], [&](const std::uint32_t i) -> const Texel& { return source.At(i, y); });
    Image out{width, height, std::vector<Texel>(static_cast<std::size_t>(width) * height)};
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            out.texels[static_cast<std::size_t>(y) * width + x] =
                blend(rows[y], [&](const std::uint32_t i) -> const Texel& { return wide.At(x, i); });
    return out;
}

std::uint16_t To565(const std::array<float, 3>& color) {
    const auto q = [](const float value, const int levels) {
        return static_cast<std::uint16_t>(std::clamp(std::lround(value / 255.0f * levels), 0l, static_cast<long>(levels)));
    };
    return static_cast<std::uint16_t>(q(color[0], 31) << 11 | q(color[1], 63) << 5 | q(color[2], 31));
}
int Distance(const Texel& a, const Texel& b) {
    int sum = 0;
    for (std::size_t c = 0; c < 3; ++c) sum += (a[c] - b[c]) * (a[c] - b[c]);
    return sum;
}
// Range fit along the principal axis of the block's colors.
void EncodeColor(const std::array<Texel, 16>& block, const bool bc1, std::byte* out) {
    const bool transparent = bc1 && std::any_of(block.begin(), block.end(), [](const Texel& t) { return t[3] < 128; });
    std::array<float, 3> mean{};
    int used = 0;
    for (const auto& t : block) {
        if (transparent && t[3] < 128) continue;
        for (std::size_t c = 0; c < 3; ++c) mean[c] += t[c];
        ++used;
    }
    std::uint16_t c0 = 0, c1 = 0;
    if (used > 0) {
        for (auto& m : mean) m /= static_cast<float>(used);
        std::array<float, 6> cov{}; // xx xy xz yy yz zz
        for (const auto& t : block) {
            if (transparent && t[3] < 128) continue;
            const float d[3]{t[0] - mean[0], t[1] - mean[1], t[2] - mean[2]};
            cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
            cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
        }
        std::array<float, 3> axis{1, 1, 1};
        for (int iteration = 0; iteration < 8; ++iteration) {
            const std::array<float, 3> next{cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                                            cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                                            cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
            const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
            if (length < 1e-6f) break;
            for (std::size_t c = 0; c < 3; ++c) axis[c] = next[c] / length;
        }
        float low = 0, high = 0;
        for (const auto& t : block) {
            if (transparent && t[3] < 128) continue;
            const float p = (t[0] - mean[0]) * axis[0] + (t[1] - mean[1]) * axis[1] + (t[2] - mean[2]) * axis[2];
            low = (std::min)(low, p);
            high = (std::max)(high, p);
        }
        std::array<float, 3> a{}, b{};
        for (std::size_t c = 0; c < 3; ++c) {
            a[c] = mean[c] + axis[c] * high;
            b[c] = mean[c] + axis[c] * low;
        }
        c0 = To565(a);
        c1 = To565(b);
    }
    // BC1 picks its mode from the endpoint order: c0 > c1 means four colors.
    if (bc1 && (transparent ? c0 > c1 : c0 < c1)) std::swap(c0, c1);
    const auto palette = ColorPalette(c0, c1, !bc1);
    std::uint32_t indices = 0;
    for (std::uint32_t i = 0; i < 16; ++i) {
        std::uint32_t best = 0;
        if (transparent && block[i][3] < 128) {
            best = 3;
        } else {
            const std::uint32_t choices = bc1 && c0 <= c1 ? 3 : 4;
            for (std::uint32_t k = 1; k < choices; ++k)
                if (Distance(block[i], palette[k]) < Distance(block[i], palette[best])) best = k;
        }
        indices |= best << (2 * i);
    }
    const std::array<std::uint8_t, 8> packed{static_cast<std::uint8_t>(c0), static_cast<std::uint8_t>(c0 >> 8),
        static_cast<std::uint8_t>(c1), static_cast<std::uint8_t>(c1 >> 8),
        static_cast<std::uint8_t>(indices), static_cast<std::uint8_t>(indices >> 8),
        static_cast<std::uint8_t>(indices >> 16), static_cast<std::uint8_t>(indices >> 24)};
    std::memcpy(out, packed.data(), packed.size());
}
void EncodeAlpha(const std::array<Texel, 16>& block, const DdsFormat format, std::byte* out) {
    std::array<std::uint8_t, 8> packed{};
    if (format == DdsFormat::bc2) {
        for (std::size_t i = 0; i < 16; ++i)
            packed[i / 2] |= static_cast<std::uint8_t>(((block[i][3] * 15 + 127) / 255) << (4 * (i % 2)));
    } else {
        std::uint8_t high = 0, low = 255;
        for (const auto& t : block) { high = (std::max)(high, t[3]); low = (std::min)(low, t[3]); }
        packed[0] = high;
        packed[1] = low;
        std::uint64_t bits = 0;
        if (high != low) {
            const auto palette = AlphaPalette(high, low);
            for (std::size_t i = 0; i < 16; ++i) {
                std::uint64_t best = 0;
                for (std::uint64_t k = 1; k < 8; ++k)
                    if (std::abs(block[i][3] - palette[k]) < std::abs(block[i][3] - palette[best])) best = k;
                bits |= best << (3 * i);
            }
        }
        for (std::size_t i = 0; i < 6; ++i) packed[2 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    std::memcpy(out, packed.data(), packed.size());
}
void Encode(const Image& image, const DdsFormat format, std::vector<std::byte>& out) {
    if (const auto block_bytes = BlockBytes(format)) {
        for (std::uint32_t by = 0; by < (image.height + 3) / 4; ++by) {
            for (std::uint32_t bx = 0; bx < (image.width + 3) / 4; ++bx) {
                std::array<Texel, 16> block;
                for (std::uint32_t i = 0; i < 16; ++i) block[i] = image.At(bx * 4 + i % 4, by * 4 + i / 4);
                const auto at = out.size();
                out.resize(at + block_bytes);
                if (format != DdsFormat::bc1) EncodeAlpha(block, format, out.data() + at);
                EncodeColor(block, format == DdsFormat::bc1, out.data() + at + (format == DdsFormat::bc1 ? 0 : 8));
            }
        }
        return;
    }
    for (const auto& t : image.texels) {
        const std::array<std::uint8_t, 4> bytes = format == DdsFormat::rgba8 ? t : Texel{t[2], t[1], t[0], t[3]};
        for (std::uint32_t c = 0; c < PixelBytes(format); ++c) out.push_back(static_cast<std::byte>(bytes[c]));
    }
}
}

bool CanConvertDdsFormat(const DdsFormat format) noexcept {
    return BlockBytes(format) != 0 || PixelBytes(format) != 0;
}
std::optional<std::vector<std::byte>> ExpandDdsToBgra8(const std::span<const std::byte> bytes,
                                                       const DdsMetadata& metadata) {
    const bool rgba = metadata.format == DdsFormat::rgba8;
    if ((!rgba && metadata.format != DdsFormat::bgr8) || metadata.has_dx10_header || metadata.header_size != 128 ||
        bytes.size() != 128 + metadata.payload_size) return std::nullopt;
    const std::size_t source_pixel = rgba ? 4 : 3, pixels = static_cast<std::size_t>(metadata.payload_size) / source_pixel;
    std::vector<std::byte> out(bytes.begin(), bytes.begin() + 128);
    out.resize(128 + pixels * 4);
    const auto put = [&out](const std::size_t offset, const std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) out[offset + i] = static_cast<std::byte>(value >> (i * 8));
    };
    // DDSD_PITCH instead of DDSD_LINEARSIZE, and the A8R8G8B8 pixel format.
    std::uint32_t flags = 0;
    for (std::size_t i = 0; i < 4; ++i) flags |= static_cast<std::uint32_t>(ByteAt(bytes, 8 + i)) << (i * 8);
    put(8, (flags | 0x8u) & ~0x80000u);
    put(20, metadata.width * 4);
    put(80, 0x41); put(84, 0); put(88, 32);
    put(92, 0x00FF0000); put(96, 0x0000FF00); put(100, 0x000000FF); put(104, 0xFF000000);
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto* in = bytes.data() + 128 + i * source_pixel;
        auto* to = out.data() + 128 + i * 4;
        to[0] = rgba ? in[2] : in[0];
        to[1] = in[1];
        to[2] = rgba ? in[0] : in[2];
        to[3] = rgba ? in[3] : std::byte{0xFF};
    }
    return out;
}

DdsConversionResult ConvertDdsToContract(const std::span<const std::byte> replacement, const DdsMetadata& source,
                                         const std::span<const std::byte> target_header, const DdsMetadata& target) {
    DdsConversionResult result;
    try {
        if (!CanConvertDdsFormat(source.format) || !CanConvertDdsFormat(target.format))
            throw std::runtime_error("format outside BC1/BC2/BC3/BGRA8/RGBA8/BGR8");
        if (source.width == 0 || source.height == 0 || target.width == 0 || target.height == 0 ||
            target.mip_count == 0 || static_cast<std::uint64_t>(source.width) * source.height > kMaxPixels ||
            static_cast<std::uint64_t>(target.width) * target.height > kMaxPixels)
            throw std::runtime_error("dimensions outside the conversion limit");
        if (target_header.size() != target.header_size || replacement.size() < source.header_size)
            throw std::runtime_error("header size mismatch");
        result.bytes.assign(target_header.begin(), target_header.end());
        result.bytes.reserve(static_cast<std::size_t>(target.expected_file_size));
        auto level = Resize(Decode(replacement, source), target.width, target.height);
        for (std::uint32_t mip = 0; mip < target.mip_count; ++mip) {
            if (mip > 0) level = Resize(level, (std::max)(1u, level.width / 2), (std::max)(1u, level.height / 2));
            Encode(level, target.format, result.bytes);
        }
        if (result.bytes.size() != target.expected_file_size)
            throw std::runtime_error("converted size differs from the original");
    } catch (const std::bad_alloc&) {
        result.bytes.clear();
        result.error = L"allocation failed";
    } catch (const std::exception& error) {
        result.bytes.clear();
        const std::string reason = error.what();
        result.error.assign(reason.begin(), reason.end());
    }
    return result;
}
}
