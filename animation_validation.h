#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ds2::modding {
// keys_in_place keeps every structural byte of a native ANM v1 clip identical:
// header (FPS, duration, flags, size), name, track flags/identifiers/weights,
// key counts, segment lengths, key times and padding. Only quantized values may
// change: rotation segment bases, steps and omitted component, rotation and
// position residuals, position segment bases, half-float scales and scalars.
enum class AnimationEditContract { keys_in_place };
struct AnimationLayout final {
    std::uint32_t fps{}, frame_count{}, flags{};
    std::uint32_t tracks{}, keys{}, value_bytes{};
    float max_rotation_magnitude{};  // largest squared sum of stored quaternion components
};
struct AnimationValidationResult final {
    AnimationLayout layout;
    std::uint32_t changed_tracks{};
    std::wstring error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// Rejects truncation, trailing bytes, unknown versions/flags, repeated key
// times, compressed quaternions outside the decoder's range and non-finite scales.
[[nodiscard]] AnimationValidationResult ParseAnimation(std::span<const std::byte> bytes);
// Edited rotation curves must also stay on or inside the unit sphere: the
// engine's reconstruction of the omitted component above 1 is unverified.
[[nodiscard]] AnimationValidationResult ValidateAnimationEdit(
    std::span<const std::byte> original, std::span<const std::byte> replacement,
    AnimationEditContract contract = AnimationEditContract::keys_in_place);
}
