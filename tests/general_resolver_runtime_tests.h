#pragma once

#include "animation_candidate.h"
#include "general_dds_candidate.h"
#include "model_candidate.h"

void TestModelRuntime(const ds2::modding::ModelCandidate& candidate,
                      std::span<const std::byte> original);
// Drives the metadata read and a member's data loop through the real detour:
// the resized member is delivered at its own size and the stream stays aligned.
void TestModelResizeRuntime(const ds2::modding::ModelCandidate& candidate,
                            std::span<const std::byte> original,
                            std::span<const std::byte> metadata_block,
                            std::span<const std::byte> next_member);
// Two resized DDS members of one segment: one metadata read patches both
// extras, each payload is delivered at its own size after its original and
// the stream stays aligned. `earlier` is what a previous member left to skip.
void TestTextureResizeRuntime(const ds2::modding::GeneralDdsCandidateSnapshot& snapshot,
                              std::span<const std::vector<std::byte>> originals,
                              std::span<const std::byte> metadata_block,
                              std::span<const std::byte> earlier,
                              std::span<const std::byte> next_member);
void TestAnimationRuntime(const ds2::modding::AnimationCandidate& candidate,
                          std::span<const std::byte> original);

void TestGeneralResolverRuntime(
    const ds2::modding::GeneralDdsCandidateSnapshot& snapshot,
    const ds2::modding::GeneralDdsCandidate& candidate,
    std::span<const std::byte> original);
