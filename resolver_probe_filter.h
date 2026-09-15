#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>

namespace ds2::modding {

inline constexpr std::size_t kTargetDdsSize = 4'224;
inline constexpr std::size_t kTargetBc3PayloadSize = 4'096;

// A cheap, allocation-free prefilter for the payload-only path. A worker
// still verifies SHA-256 before treating a sample as the target asset.
inline constexpr std::uint64_t kOriginalBc3PayloadFnv1a64 =
    0x40A4729137EEC844ull;

enum class ResolverSampleKind {
    none,
    full_dds,
    target_bc3_payload_candidate,
};

[[nodiscard]] std::uint64_t Fnv1a64(
    std::span<const std::byte> bytes) noexcept;

// A self-contained SHA-256 used only on fixed in-memory snapshots inside the
// internal detour. It performs no allocation and calls no OS or crypto APIs.
struct PortableSha256Context final {
    std::array<std::uint32_t, 8> state{
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };
    std::array<std::byte, 64> buffer{};
    std::size_t buffered_size{};
    std::uint64_t total_size{};
    bool valid{true};
};

// Incremental form used when process memory must be copied in bounded chunks.
// The context becomes invalid if the SHA-256 64-bit bit-length limit would be
// exceeded. Finalization consumes a copy, so callers can retain their context.
[[nodiscard]] bool PortableSha256Update(
    PortableSha256Context& context,
    std::span<const std::byte> bytes) noexcept;
[[nodiscard]] bool PortableSha256Finalize(
    PortableSha256Context context,
    std::array<std::uint8_t, 32>& digest) noexcept;

[[nodiscard]] std::array<std::uint8_t, 32> PortableSha256(
    std::span<const std::byte> bytes) noexcept;

// Classifies a stable local snapshot taken after the original stream read.
// Full DDS samples are intentionally broad and are SHA-256 filtered later.
[[nodiscard]] ResolverSampleKind ClassifyResolverSample(
    std::int32_t requested,
    std::int32_t returned,
    std::span<const std::byte> bytes) noexcept;

}  // namespace ds2::modding
