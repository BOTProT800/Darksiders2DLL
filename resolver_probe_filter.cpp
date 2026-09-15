#include "resolver_probe_filter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace ds2::modding {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

[[nodiscard]] constexpr std::uint32_t RotateRight(
    const std::uint32_t value,
    const unsigned int count) noexcept {
    return (value >> count) | (value << (32u - count));
}

void TransformSha256(
    std::array<std::uint32_t, 8>& state,
    const std::byte* const block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t offset = index * 4;
        words[index] =
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset])) << 24) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 1])) << 16) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 2])) << 8) |
            static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset + 3]));
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const std::uint32_t previous15 = words[index - 15];
        const std::uint32_t previous2 = words[index - 2];
        const std::uint32_t sigma0 = RotateRight(previous15, 7) ^
            RotateRight(previous15, 18) ^ (previous15 >> 3);
        const std::uint32_t sigma1 = RotateRight(previous2, 17) ^
            RotateRight(previous2, 19) ^ (previous2 >> 10);
        words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::uint32_t sum1 = RotateRight(e, 6) ^
            RotateRight(e, 11) ^ RotateRight(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temporary1 =
            h + sum1 + choose + kSha256RoundConstants[index] + words[index];
        const std::uint32_t sum0 = RotateRight(a, 2) ^
            RotateRight(a, 13) ^ RotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::uint64_t Fnv1a64(const std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = 0xCBF29CE484222325ull;
    for (const std::byte byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= 0x100000001B3ull;
    }
    return value;
}

bool PortableSha256Update(
    PortableSha256Context& context,
    const std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t kMaximumMessageBytes =
        (std::numeric_limits<std::uint64_t>::max)() / 8ull;
    if (!context.valid || context.buffered_size >= context.buffer.size() ||
        context.total_size > kMaximumMessageBytes ||
        static_cast<std::uint64_t>(bytes.size()) >
            kMaximumMessageBytes - context.total_size) {
        context.valid = false;
        return false;
    }
    context.total_size += static_cast<std::uint64_t>(bytes.size());

    std::size_t offset = 0;
    if (context.buffered_size != 0) {
        const std::size_t copied = (std::min)(
            bytes.size(), context.buffer.size() - context.buffered_size);
        if (copied != 0) {
            std::memcpy(
                context.buffer.data() + context.buffered_size,
                bytes.data(), copied);
            context.buffered_size += copied;
            offset = copied;
        }
        if (context.buffered_size == context.buffer.size()) {
            TransformSha256(context.state, context.buffer.data());
            context.buffered_size = 0;
        } else {
            return true;
        }
    }

    while (bytes.size() - offset >= 64) {
        TransformSha256(context.state, bytes.data() + offset);
        offset += 64;
    }

    const std::size_t remaining = bytes.size() - offset;
    if (remaining != 0) {
        std::memcpy(context.buffer.data(), bytes.data() + offset, remaining);
    }
    context.buffered_size = remaining;
    return true;
}

bool PortableSha256Finalize(
    PortableSha256Context context,
    std::array<std::uint8_t, 32>& digest) noexcept {
    if (!context.valid || context.buffered_size >= context.buffer.size()) {
        digest = {};
        return false;
    }

    const std::uint64_t bit_length = context.total_size * 8ull;
    context.buffer[context.buffered_size++] = std::byte{0x80};
    if (context.buffered_size > 56) {
        std::fill(
            context.buffer.begin() + context.buffered_size,
            context.buffer.end(), std::byte{});
        TransformSha256(context.state, context.buffer.data());
        context.buffer.fill(std::byte{});
    } else {
        std::fill(
            context.buffer.begin() + context.buffered_size,
            context.buffer.begin() + 56, std::byte{});
    }
    for (std::size_t index = 0; index < 8; ++index) {
        context.buffer[63 - index] = static_cast<std::byte>(
            (bit_length >> (index * 8)) & 0xFFu);
    }
    TransformSha256(context.state, context.buffer.data());

    for (std::size_t index = 0; index < context.state.size(); ++index) {
        digest[index * 4] = static_cast<std::uint8_t>(
            context.state[index] >> 24);
        digest[index * 4 + 1] = static_cast<std::uint8_t>(
            context.state[index] >> 16);
        digest[index * 4 + 2] = static_cast<std::uint8_t>(
            context.state[index] >> 8);
        digest[index * 4 + 3] = static_cast<std::uint8_t>(
            context.state[index]);
    }
    return true;
}

std::array<std::uint8_t, 32> PortableSha256(
    const std::span<const std::byte> bytes) noexcept {
    PortableSha256Context context;
    std::array<std::uint8_t, 32> digest{};
    if (!PortableSha256Update(context, bytes) ||
        !PortableSha256Finalize(context, digest)) {
        return {};
    }
    return digest;
}

ResolverSampleKind ClassifyResolverSample(
    const std::int32_t requested,
    const std::int32_t returned,
    const std::span<const std::byte> bytes) noexcept {
    if (requested != returned || requested <= 0 ||
        bytes.size() != static_cast<std::size_t>(requested)) {
        return ResolverSampleKind::none;
    }

    if (bytes.size() == kTargetDdsSize &&
        bytes[0] == std::byte{0x44} &&
        bytes[1] == std::byte{0x44} &&
        bytes[2] == std::byte{0x53} &&
        bytes[3] == std::byte{0x20}) {
        return ResolverSampleKind::full_dds;
    }

    if (bytes.size() == kTargetBc3PayloadSize &&
        Fnv1a64(bytes) == kOriginalBc3PayloadFnv1a64) {
        return ResolverSampleKind::target_bc3_payload_candidate;
    }

    return ResolverSampleKind::none;
}

}  // namespace ds2::modding
