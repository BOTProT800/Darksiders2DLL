#pragma once

#include "general_dds_candidate.h"
#include "model_candidate.h"

void TestModelRuntime(const ds2::modding::ModelCandidate& candidate,
                      std::span<const std::byte> original);

void TestGeneralResolverRuntime(
    const ds2::modding::GeneralDdsCandidateSnapshot& snapshot,
    const ds2::modding::GeneralDdsCandidate& candidate,
    std::span<const std::byte> original);
