#pragma once
#include "inventory_script.h"

namespace ds2::modding {
struct InventoryScriptHookStats final {
    std::uint64_t matched{}, redirected{}, observed{}, failures{};
    bool active{};
    int minhook_status{};
};
// Caller authenticates the executable and pins this DLL before installation.
// Ownership is retained for process lifetime, including partial hook failures.
[[nodiscard]] bool InitializeInventoryScriptHook(
    std::shared_ptr<const InventoryScriptCandidate> candidate, bool write_enabled) noexcept;
[[nodiscard]] InventoryScriptHookStats GetInventoryScriptHookStats() noexcept;
}
