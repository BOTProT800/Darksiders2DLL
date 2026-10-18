#pragma once
#include <filesystem>
void TestNativeTextures(const std::filesystem::path& root);
void TestTextureConversion();
int CheckTextureFiles(const std::filesystem::path& source);
// OUTPUT, when given, receives each candidate's delivered DDS (converted or not).
int CheckTextureCatalog(const std::filesystem::path& game, const std::filesystem::path& mods,
                        const std::filesystem::path& output = {});
