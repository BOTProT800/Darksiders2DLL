#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// Synthetic ANM v1 clip covering every track flag bit, inline names, hashed
// names with alignment, multi-segment interpolation and an empty rotation.
struct AnimationFixture final {
    std::vector<std::byte> bytes;
    std::vector<std::uint8_t> mutable_bits;  // expectation recorded by the writer
    std::vector<std::size_t> track_ends;
    std::vector<std::size_t> later_times;    // time deltas after each curve's first key
    std::size_t unit_residuals{};            // residuals of the one-key rotation
    std::size_t scale_values{};
};
AnimationFixture MakeTestAnimation();
void TestAnimations(const std::filesystem::path& temporary_root);
int CheckAnimationFiles(const std::filesystem::path& original, const std::filesystem::path& modified);
int CheckAnimationCatalog(const std::filesystem::path& game, const std::filesystem::path& mods);
int ScanAnimationCorpus(const std::filesystem::path& directory, const std::filesystem::path& report);
