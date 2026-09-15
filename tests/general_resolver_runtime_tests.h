#pragma once

#include "general_dds_candidate.h"

void TestGeneralResolverRuntime(
    const ds2::modding::GeneralDdsCandidateSnapshot& snapshot,
    const ds2::modding::GeneralDdsCandidate& candidate,
    std::span<const std::byte> original);
