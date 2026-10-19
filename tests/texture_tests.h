#pragma once
#include <filesystem>
void TestNativeTextures(const std::filesystem::path& root);
void TestTextureConversion();
// Extra encoding, level sizes, the handler's mip trimming and the resize plan.
void TestTextureResize();
int CheckTextureFiles(const std::filesystem::path& source);
// OUTPUT, when given, receives each candidate's delivered DDS (converted or not).
// RESIZE also plans textures=resize and fails when a texture of another
// contract gets no plan.
int CheckTextureCatalog(const std::filesystem::path& game, const std::filesystem::path& mods,
                        const std::filesystem::path& output = {}, bool resize = false);
