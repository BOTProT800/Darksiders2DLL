#pragma once
#include <filesystem>
void TestNativeTextures(const std::filesystem::path& root);
int CheckTextureFiles(const std::filesystem::path& source);
int CheckTextureCatalog(const std::filesystem::path& game, const std::filesystem::path& mods);
