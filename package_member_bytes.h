#pragma once
#include "package_dds_contract.h"

namespace ds2::modding {
struct PackageMemberBytes final {
    PackageResourceIdentity identity;
    std::shared_ptr<const std::vector<std::byte>> bytes;
};
struct PackageMemberReadResult final {
    std::vector<PackageMemberBytes> members;
    // Reuse the existing bounded package reader's error/issue vocabulary.
    PackageDdsContractCatalogBuildResult diagnostics;
    [[nodiscard]] explicit operator bool() const noexcept {
        return diagnostics.error == PackageDdsContractCatalogError::none &&
               diagnostics.issues.empty();
    }
};
[[nodiscard]] PackageMemberReadResult ReadMediaPackageMembers(
    const std::filesystem::path& game_directory,
    std::span<const PackageIdentityEntry> entries,
    const PackageDdsContractCatalogOptions& limits = {});
}
