#pragma once
#include <cstddef>
#include <filesystem>
#include <vector>

std::vector<std::byte> MakeTestModel(bool skinned = false);
void TestModels(const std::filesystem::path& temporary_root);
// A smaller MODIFIED is checked as a deletion; EXPANDED receives the rebuilt file.
int CheckModelFiles(const std::filesystem::path& original, const std::filesystem::path& modified,
                    const std::filesystem::path& expanded = {});
int CheckModelCatalog(const std::filesystem::path& game, const std::filesystem::path& mods);
