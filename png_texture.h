#pragma once
#include "byte_storage.h"

namespace ds2::modding {
// Decode only PNG, keep straight alpha and channel values, and generate a full
// box-filtered mip chain in a traditional BGRA8 DDS. No color-profile transform.
// The budget includes the DDS header and every mip, checked before allocation.
[[nodiscard]] ByteLoadResult DecodePngTexture(
    std::span<const std::byte> png, std::uint64_t max_bytes);
}
