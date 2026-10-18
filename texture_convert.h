#pragma once
#include "dds_validation.h"
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ds2::modding {
// BC1, BC2, BC3, BGRA8, RGBA8 and BGR8 decode and encode; BC4/BC5 do not.
[[nodiscard]] bool CanConvertDdsFormat(DdsFormat format) noexcept;

struct DdsConversionResult final {
    std::vector<std::byte> bytes; // target header + converted payload
    std::wstring error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// Decodes the replacement's top level, resamples it to the target size,
// rebuilds the target mip chain and encodes it in the target format. The
// target header is copied verbatim, so the result has the original's exact
// size and layout and can take the same-size stream route. Block formats are
// re-encoded, which is lossy. At most 4096x4096 on either side.
[[nodiscard]] DdsConversionResult ConvertDdsToContract(
    std::span<const std::byte> replacement, const DdsMetadata& source,
    std::span<const std::byte> target_header, const DdsMetadata& target);
}
