#pragma once
#include <filesystem>
void TestInventoryScripts(const std::filesystem::path& root);
int CheckInventoryScriptMod(const std::filesystem::path& game, const std::filesystem::path& mods);
