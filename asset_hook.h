#pragma once

#include "byte_storage.h"

#include <cstdint>
#include <memory>

namespace ds2::modding {

// Proof-of-concept texture interception for the single first_test_mod asset.
// This is deliberately not a general asset resolver: unknown inputs always
// continue to the original D3DX function unchanged.
enum class TextureHookState {
    not_initialized,
    invalid_replacement,
    waiting_for_d3dx,
    active,
    installation_failed,
    indeterminate,
    stopped,
};

struct TextureHookStatus final {
    TextureHookState state{TextureHookState::not_initialized};
    bool d3dx11_hook_installed{};
    bool d3dx9_hook_installed{};
    int last_minhook_status{};
};

struct TextureHookStats final {
    std::uint64_t d3dx11_calls{};
    std::uint64_t d3dx9_calls{};
    std::uint64_t size_candidates{};
    std::uint64_t original_hash_matches{};
    std::uint64_t replacements_applied{};
    std::uint64_t hash_failures{};
    std::uint64_t reentrant_bypasses{};
    std::uint64_t asynchronous_bypasses{};
    std::uint64_t passthrough_calls{};
    std::uint64_t installation_failures{};
};

enum class TextureHookEvent {
    d3dx11_first_entry,
    d3dx9_first_entry,
    original_asset_matched,
    replacement_applied,
};

// The callback runs at most once per event kind and must only enqueue/signal
// POD state. It executes inside the detour; filesystem I/O belongs on a worker.
using TextureHookEventCallback = void (*)(TextureHookEvent event) noexcept;

void SetTextureHookEventCallback(TextureHookEventCallback callback) noexcept;

// Validates and publishes an immutable 64x64 BC3, one-mip, 4224-byte DDS,
// then hooks whichever supported D3DX entry points are already loaded.
// Calling this again is safe and retries entry points that were not available
// earlier. Returns true when at least one hook is active.
[[nodiscard]] bool InitializeFirstTextureOverride(
    std::shared_ptr<const ByteStorage> replacement) noexcept;

// Snapshot-only diagnostics intended for bootstrap/session logging. The hook
// itself performs no logging or disk I/O.
[[nodiscard]] TextureHookStatus GetTextureHookStatus() noexcept;
[[nodiscard]] TextureHookStats GetTextureHookStats() noexcept;

// Terminal process-lifetime shutdown: disables owned hooks but deliberately
// keeps trampolines and replacement storage pinned for in-flight calls.
// MinHook itself is not uninitialized because other hooks may share it.
void ShutdownTextureHooks() noexcept;

}  // namespace ds2::modding
