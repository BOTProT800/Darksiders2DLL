#include "animation_validation.h"
// Native ANM v1 layout reference: Anansi/services/darksiders/animation.py,
// Copyright (c) 2026 BOTProT800, MIT. See THIRD_PARTY_NOTICES.md.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace ds2::modding {
namespace {
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kMaxAnimationBytes = 64ull * 1024 * 1024;
constexpr float kRotationUnit = 8.632728713564575e-05f;
constexpr std::array<std::int64_t, 4> kRotationSteps{1, 2, 4, 16};

struct Span final {
    std::size_t begin{}, end{};
    bool operator==(const Span&) const = default;
};
struct RotationCurve final {
    Span bytes;
    float max_magnitude{};
};
struct Parsed final {
    AnimationLayout layout;
    // Per byte: the bits a keys_in_place edit may change.
    std::vector<std::uint8_t> mutable_bits;
    std::vector<Span> tracks;
    std::vector<RotationCurve> rotations;
};
enum class CurveKind { rotation, position, scale, scalar };

class Reader final {
public:
    Reader(const std::span<const std::byte> bytes, Parsed& parsed) : bytes_(bytes), parsed_(parsed) {}
    [[nodiscard]] std::size_t At() const noexcept { return at_; }
    [[nodiscard]] bool Done() const noexcept { return at_ == bytes_.size(); }
    std::size_t Take(const std::size_t size) {
        if (size > bytes_.size() - at_) throw std::runtime_error("truncated animation");
        const auto start = at_;
        at_ += size;
        return start;
    }
    [[nodiscard]] std::uint8_t U8At(const std::size_t offset) const noexcept {
        return static_cast<std::uint8_t>(bytes_[offset]);
    }
    [[nodiscard]] std::uint16_t U16At(const std::size_t offset) const noexcept {
        return static_cast<std::uint16_t>(U8At(offset) | (U8At(offset + 1) << 8));
    }
    std::uint8_t U8() { return U8At(Take(1)); }
    std::uint16_t U16() { return U16At(Take(2)); }
    void Align(const std::size_t size) { Take((size - at_ % size) % size); }
    void String() {
        std::size_t end = at_;
        while (end < bytes_.size() && bytes_[end] != std::byte{}) ++end;
        if (end == bytes_.size()) throw std::runtime_error("unterminated animation string");
        Take(end - at_ + 1);
    }
    void MarkMutable(const std::size_t offset, const std::size_t size, const std::uint8_t bits = 0xFF) {
        std::fill_n(parsed_.mutable_bits.begin() + static_cast<std::ptrdiff_t>(offset), size, bits);
        if (bits == 0xFF) parsed_.layout.value_bytes += static_cast<std::uint32_t>(size);
    }

private:
    std::span<const std::byte> bytes_;
    Parsed& parsed_;
    std::size_t at_{kHeaderSize};
};

struct Segment final {
    std::size_t offset{};
    std::uint32_t start{}, run{};
};

std::int64_t SignedBase(const std::uint8_t value) noexcept {
    return static_cast<std::int64_t>(((value & 63) ^ 32)) - 32;
}

// Mirrors the decoder's float32 arithmetic so values at the magnitude limit are
// classified exactly as the reference parser classifies them.
float RotationMagnitude(const Reader& reader, const std::vector<Segment>& segments,
                        const std::size_t residuals, const std::vector<std::int64_t>& times,
                        const std::uint8_t track_flags) {
    float largest = 0;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const auto& segment = segments[index];
        const auto& following = segments[(std::min)(index + 1, segments.size() - 1)];
        std::array<std::int64_t, 3> base{}, next_base{}, step{};
        for (std::size_t c = 0; c < 3; ++c) {
            const auto value = reader.U8At(segment.offset + c);
            base[c] = SignedBase(value);
            step[c] = kRotationSteps[value >> 6];
            next_base[c] = SignedBase(reader.U8At(following.offset + c));
        }
        const auto first = times[segment.start];
        const auto end_time = times[(std::min<std::size_t>)(segment.start + segment.run, times.size() - 1)];
        const auto span = (std::max<std::int64_t>)(1, end_time - first);
        for (std::uint32_t key = segment.start; key < segment.start + segment.run; ++key) {
            const float fraction = static_cast<float>(times[key] - first) * 256.0f / static_cast<float>(span);
            float magnitude = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const auto interpolated = static_cast<std::int64_t>(
                    std::trunc(fraction * static_cast<float>(next_base[c] - base[c])));
                const auto residual = static_cast<std::int8_t>(reader.U8At(residuals + key * 3ull + c));
                const std::int64_t integer = base[c] * 256 + interpolated + residual * step[c] +
                    ((track_flags & 32) != 0 ? 128 : 0);
                const float component = static_cast<float>(static_cast<std::int16_t>(integer)) * kRotationUnit;
                magnitude += component * component;
            }
            if (!(magnitude <= 1.0001f)) throw std::runtime_error("invalid compressed quaternion");
            largest = (std::max)(largest, magnitude);
        }
    }
    return largest;
}

void ParseCurve(Reader& reader, Parsed& parsed, const CurveKind kind, const std::uint8_t track_flags) {
    const auto begin = reader.At();
    const auto count = reader.U16();
    if (count == 0) return;
    std::vector<Segment> segments;
    std::size_t values{};
    if (kind == CurveKind::rotation || kind == CurveKind::position) {
        std::uint32_t used = 0;
        while (used < count) {
            const bool rotation = kind == CurveKind::rotation;
            const auto offset = reader.Take(rotation ? 4 : 8);
            const std::uint32_t run = rotation ? (reader.U8At(offset + 3) & 63u) + 1u : reader.U16At(offset);
            if (run == 0 || run > count - used) throw std::runtime_error("invalid animation segment");
            segments.push_back({offset, used, run});
            if (rotation) {
                reader.MarkMutable(offset, 3);
                reader.MarkMutable(offset + 3, 1, 0xC0);  // omitted component; run length stays
            } else {
                reader.MarkMutable(offset + 2, 6);        // base; the u16 run length stays
            }
            used += run;
        }
        values = reader.Take(count * 3ull);
        reader.MarkMutable(values, count * 3ull);
    } else if (kind == CurveKind::scale) {
        values = reader.Take(count * 6ull);
        for (std::size_t i = 0; i < count * 3ull; ++i)
            if ((reader.U16At(values + i * 2) & 0x7C00u) == 0x7C00u)
                throw std::runtime_error("non-finite animation scale");
        reader.MarkMutable(values, count * 6ull);
    } else {
        values = reader.Take(count);
        reader.MarkMutable(values, count);
    }
    const auto deltas = reader.Take(count);
    std::vector<std::int64_t> times(count);
    std::int64_t time = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto delta = reader.U8At(deltas + i);
        if (i > 0 && delta == 0) throw std::runtime_error("repeated animation key time");
        time += delta;
        times[i] = time;
    }
    reader.Align(2);
    parsed.layout.keys += count;
    if (kind == CurveKind::rotation) {
        const auto magnitude = RotationMagnitude(reader, segments, values, times, track_flags);
        parsed.rotations.push_back({{begin, reader.At()}, magnitude});
        parsed.layout.max_rotation_magnitude = (std::max)(parsed.layout.max_rotation_magnitude, magnitude);
    }
}

Parsed Parse(const std::span<const std::byte> bytes) {
    if (bytes.size() <= kHeaderSize || bytes.size() > kMaxAnimationBytes)
        throw std::runtime_error("animation size");
    const auto u8 = [&](const std::size_t offset) { return static_cast<std::uint8_t>(bytes[offset]); };
    std::uint16_t frames{};
    std::uint32_t size{}, flags{};
    std::memcpy(&frames, bytes.data() + 6, sizeof(frames));
    std::memcpy(&size, bytes.data() + 8, sizeof(size));
    std::memcpy(&flags, bytes.data() + 12, sizeof(flags));
    if (u8(0) != 'A' || u8(1) != 'N' || u8(2) != 'M' || u8(3) != 1 || u8(4) != 0)
        throw std::runtime_error("not an ANM v1 little-endian clip");
    if (u8(5) == 0 || frames == 0 || size != bytes.size() - kHeaderSize || (flags & ~3u) != 0)
        throw std::runtime_error("invalid animation header");
    Parsed parsed;
    parsed.layout = {u8(5), frames, flags};
    parsed.mutable_bits.assign(bytes.size(), 0);
    Reader reader{bytes, parsed};
    reader.String();
    while (!reader.Done()) {
        const auto begin = reader.At();
        const auto track_flags = reader.U8();
        if ((track_flags & ~63u) != 0) throw std::runtime_error("unsupported animation track flags");
        if ((track_flags & 1) != 0) {
            reader.String();
        } else {
            reader.Align(8);
            reader.Take(8);  // bone name hash
        }
        reader.Align(2);
        reader.Take(2);  // weight
        ParseCurve(reader, parsed, CurveKind::rotation, track_flags);
        if ((track_flags & 2) != 0) ParseCurve(reader, parsed, CurveKind::position, track_flags);
        if ((track_flags & 4) != 0) ParseCurve(reader, parsed, CurveKind::scale, track_flags);
        if ((track_flags & 8) != 0) ParseCurve(reader, parsed, CurveKind::scalar, track_flags);
        parsed.tracks.push_back({begin, reader.At()});
    }
    if (parsed.tracks.empty()) throw std::runtime_error("animation has no tracks");
    parsed.layout.tracks = static_cast<std::uint32_t>(parsed.tracks.size());
    return parsed;
}

std::wstring Widen(const std::string& text) { return {text.begin(), text.end()}; }

bool SameBytes(const std::span<const std::byte> left, const std::span<const std::byte> right, const Span span) {
    return std::equal(left.begin() + static_cast<std::ptrdiff_t>(span.begin),
                      left.begin() + static_cast<std::ptrdiff_t>(span.end),
                      right.begin() + static_cast<std::ptrdiff_t>(span.begin));
}
}

AnimationValidationResult ParseAnimation(const std::span<const std::byte> bytes) {
    AnimationValidationResult result;
    try {
        result.layout = Parse(bytes).layout;
    } catch (const std::exception& error) {
        result.error = Widen(error.what());
    }
    return result;
}

AnimationValidationResult ValidateAnimationEdit(
    const std::span<const std::byte> original,
    const std::span<const std::byte> replacement,
    const AnimationEditContract contract) {
    AnimationValidationResult result;
    if (contract != AnimationEditContract::keys_in_place) {
        result.error = L"unsupported animation contract";
        return result;
    }
    if (original.size() != replacement.size()) {
        result.error = L"animation size changed; keys_in_place keeps the original structure";
        return result;
    }
    Parsed before, after;
    try {
        before = Parse(original);
    } catch (const std::exception& error) {
        result.error = L"original: " + Widen(error.what());
        return result;
    }
    try {
        after = Parse(replacement);
    } catch (const std::exception& error) {
        result.error = L"replacement: " + Widen(error.what());
        return result;
    }
    for (std::size_t i = 0; i < original.size(); ++i) {
        const auto changed = static_cast<std::uint8_t>(original[i] ^ replacement[i]);
        if ((changed & static_cast<std::uint8_t>(~before.mutable_bits[i])) != 0) {
            result.error = L"immutable animation bytes changed (header/name/tracks/key counts/"
                           L"segment lengths/times) at offset " + std::to_wstring(i);
            return result;
        }
    }
    // Identical structural bytes must reproduce the same layout; check anyway.
    if (after.tracks != before.tracks || after.rotations.size() != before.rotations.size() ||
        after.layout.keys != before.layout.keys) {
        result.error = L"animation layout differs despite identical structure";
        return result;
    }
    for (std::size_t i = 0; i < after.rotations.size(); ++i) {
        const auto& rotation = after.rotations[i];
        if (rotation.bytes != before.rotations[i].bytes) {
            result.error = L"animation layout differs despite identical structure";
            return result;
        }
        if (!SameBytes(original, replacement, rotation.bytes) && rotation.max_magnitude > 1.0f) {
            result.error = L"edited rotation leaves the unit sphere at offset " +
                           std::to_wstring(rotation.bytes.begin);
            return result;
        }
    }
    for (const auto& track : before.tracks)
        if (!SameBytes(original, replacement, track)) ++result.changed_tracks;
    result.layout = before.layout;
    return result;
}
}
