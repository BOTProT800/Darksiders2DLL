#include "pch.h"

#include "asset_hook.h"

#include "dds_validation.h"
#include "memory_range.h"
#include "sha256.h"

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

namespace ds2::modding {
namespace {

constexpr std::size_t kExpectedDdsSize = 4'224;

constexpr Sha256Digest kOriginalSha256{
    0xE2, 0x17, 0x3F, 0x05, 0xC5, 0x75, 0x67, 0x77,
    0x56, 0x07, 0x1A, 0xD7, 0xDE, 0xC8, 0x8E, 0x4C,
    0xF4, 0x9B, 0xBD, 0x0A, 0x73, 0x4F, 0x90, 0x21,
    0x59, 0xE9, 0x3C, 0x79, 0xF1, 0x91, 0xB2, 0x2D,
};

constexpr Sha256Digest kEditedSha256{
    0x2C, 0x0D, 0x7E, 0x05, 0x0F, 0x84, 0x6A, 0x63,
    0x37, 0xA3, 0x39, 0x82, 0xFB, 0x22, 0x1C, 0x0F,
    0x38, 0xA0, 0xE2, 0x65, 0xD5, 0xFA, 0xCF, 0x45,
    0x07, 0x4B, 0x7D, 0x02, 0xEE, 0x9C, 0xB7, 0x19,
};

// ABI-equivalent declarations without the retired D3DX headers. All COM and
// D3DX structure parameters are opaque pointers. D3DFORMAT and D3DPOOL are
// 32-bit enums, represented here as int32_t. WINAPI is significant on x86 and
// documents the actual API even though Windows x64 has one calling convention.
using D3dx11CreateTextureFromMemoryFn = HRESULT(WINAPI*)(
    void* device,
    const void* source_data,
    SIZE_T source_size,
    const void* load_info,
    void* thread_pump,
    void** texture,
    HRESULT* result);

using D3dx9CreateTextureFromFileInMemoryExFn = HRESULT(WINAPI*)(
    void* device,
    const void* source_data,
    UINT source_size,
    UINT width,
    UINT height,
    UINT mip_levels,
    DWORD usage,
    std::int32_t format,
    std::int32_t pool,
    DWORD filter,
    DWORD mip_filter,
    DWORD color_key,
    void* source_info,
    void* palette,
    void** texture);

std::mutex g_lifecycle_mutex;
std::atomic<std::shared_ptr<const ByteStorage>> g_replacement;

std::atomic<D3dx11CreateTextureFromMemoryFn> g_original_d3dx11{};
std::atomic<D3dx9CreateTextureFromFileInMemoryExFn> g_original_d3dx9{};
void* g_d3dx11_target{};
void* g_d3dx9_target{};
bool g_d3dx11_created{};
bool g_d3dx9_created{};

std::atomic<TextureHookState> g_state{TextureHookState::not_initialized};
std::atomic<bool> g_d3dx11_installed{};
std::atomic<bool> g_d3dx9_installed{};
std::atomic<int> g_last_minhook_status{static_cast<int>(MH_OK)};

std::atomic<std::uint64_t> g_d3dx11_calls{};
std::atomic<std::uint64_t> g_d3dx9_calls{};
std::atomic<std::uint64_t> g_size_candidates{};
std::atomic<std::uint64_t> g_original_hash_matches{};
std::atomic<std::uint64_t> g_replacements_applied{};
std::atomic<std::uint64_t> g_hash_failures{};
std::atomic<std::uint64_t> g_reentrant_bypasses{};
std::atomic<std::uint64_t> g_asynchronous_bypasses{};
std::atomic<std::uint64_t> g_passthrough_calls{};
std::atomic<std::uint64_t> g_installation_failures{};
std::atomic<TextureHookEventCallback> g_event_callback{};
std::atomic<bool> g_reported_d3dx11_entry{};
std::atomic<bool> g_reported_d3dx9_entry{};
std::atomic<bool> g_reported_original_match{};
std::atomic<bool> g_reported_replacement{};

thread_local bool g_inside_texture_hook = false;

class ReentrancyScope final {
public:
    ReentrancyScope() noexcept { g_inside_texture_hook = true; }
    ~ReentrancyScope() { g_inside_texture_hook = false; }

    ReentrancyScope(const ReentrancyScope&) = delete;
    ReentrancyScope& operator=(const ReentrancyScope&) = delete;
};

void NotifyOnce(
    const TextureHookEvent event,
    std::atomic<bool>& reported) noexcept {
    const auto callback = g_event_callback.load(std::memory_order_acquire);
    if (callback == nullptr) {
        return;
    }
    bool expected = false;
    if (reported.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        callback(event);
    }
}

[[nodiscard]] bool ValidateReplacement(
    const std::shared_ptr<const ByteStorage>& replacement) noexcept {
    if (!replacement || replacement->Size() != kExpectedDdsSize ||
        replacement->Sha256() != kEditedSha256) {
        return false;
    }

    DdsExpectations expectations;
    expectations.width = 64;
    expectations.height = 64;
    expectations.mip_count = 1;
    expectations.format = DdsFormat::bc3;
    expectations.exact_file_size = kExpectedDdsSize;

    try {
        return static_cast<bool>(ValidateDds(replacement->Bytes(), expectations));
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool SelectReplacement(
    const void* source_data,
    const std::size_t source_size,
    const void*& selected_data,
    std::size_t& selected_size,
    std::shared_ptr<const ByteStorage>& keep_alive) noexcept {
    selected_data = source_data;
    selected_size = source_size;

    if (source_data == nullptr || source_size != kExpectedDdsSize) {
        return false;
    }

    g_size_candidates.fetch_add(1, std::memory_order_relaxed);

    if (!IsReadableMemoryRange(source_data, kExpectedDdsSize)) {
        g_hash_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Hash a stable local snapshot. ReadProcessMemory probes the complete
    // source range and reports a failed/partial copy instead of letting an
    // access violation escape a noexcept detour compiled with /EHsc.
    std::array<std::byte, kExpectedDdsSize> source_snapshot{};
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            source_data,
            source_snapshot.data(),
            source_snapshot.size(),
            &bytes_read) ||
        bytes_read != source_snapshot.size()) {
        g_hash_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Sha256Result hash;
    try {
        hash = ComputeSha256(source_snapshot);
    } catch (...) {
        g_hash_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (!hash) {
        g_hash_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (hash.digest != kOriginalSha256) {
        return false;
    }

    g_original_hash_matches.fetch_add(1, std::memory_order_relaxed);
    NotifyOnce(TextureHookEvent::original_asset_matched, g_reported_original_match);
    keep_alive = g_replacement.load(std::memory_order_acquire);
    if (!keep_alive) {
        return false;
    }

    selected_data = keep_alive->Data();
    selected_size = keep_alive->Size();
    return true;
}

HRESULT WINAPI HookD3dx11CreateTextureFromMemory(
    void* device,
    const void* source_data,
    const SIZE_T source_size,
    const void* load_info,
    void* thread_pump,
    void** texture,
    HRESULT* result) noexcept {
    g_d3dx11_calls.fetch_add(1, std::memory_order_relaxed);
    NotifyOnce(TextureHookEvent::d3dx11_first_entry, g_reported_d3dx11_entry);
    const auto original = g_original_d3dx11.load(std::memory_order_acquire);
    if (original == nullptr) {
        return E_FAIL;
    }

    if (g_inside_texture_hook) {
        g_reentrant_bypasses.fetch_add(1, std::memory_order_relaxed);
        g_passthrough_calls.fetch_add(1, std::memory_order_relaxed);
        return original(
            device, source_data, source_size, load_info, thread_pump, texture, result);
    }

    ReentrancyScope scope;
    const void* selected_data{};
    std::size_t selected_size{};
    std::shared_ptr<const ByteStorage> replacement_lifetime;
    bool replaced = false;
    if (thread_pump == nullptr) {
        replaced = SelectReplacement(
            source_data,
            static_cast<std::size_t>(source_size),
            selected_data,
            selected_size,
            replacement_lifetime);
    } else {
        selected_data = source_data;
        selected_size = static_cast<std::size_t>(source_size);
        g_asynchronous_bypasses.fetch_add(1, std::memory_order_relaxed);
    }
    if (!replaced) {
        g_passthrough_calls.fetch_add(1, std::memory_order_relaxed);
    }

    const HRESULT call_result = original(
        device,
        selected_data,
        static_cast<SIZE_T>(selected_size),
        load_info,
        thread_pump,
        texture,
        result);
    if (replaced && SUCCEEDED(call_result)) {
        g_replacements_applied.fetch_add(1, std::memory_order_relaxed);
        NotifyOnce(TextureHookEvent::replacement_applied, g_reported_replacement);
    }
    return call_result;
}

HRESULT WINAPI HookD3dx9CreateTextureFromFileInMemoryEx(
    void* device,
    const void* source_data,
    const UINT source_size,
    const UINT width,
    const UINT height,
    const UINT mip_levels,
    const DWORD usage,
    const std::int32_t format,
    const std::int32_t pool,
    const DWORD filter,
    const DWORD mip_filter,
    const DWORD color_key,
    void* source_info,
    void* palette,
    void** texture) noexcept {
    g_d3dx9_calls.fetch_add(1, std::memory_order_relaxed);
    NotifyOnce(TextureHookEvent::d3dx9_first_entry, g_reported_d3dx9_entry);
    const auto original = g_original_d3dx9.load(std::memory_order_acquire);
    if (original == nullptr) {
        return E_FAIL;
    }

    if (g_inside_texture_hook) {
        g_reentrant_bypasses.fetch_add(1, std::memory_order_relaxed);
        g_passthrough_calls.fetch_add(1, std::memory_order_relaxed);
        return original(
            device, source_data, source_size, width, height, mip_levels, usage,
            format, pool, filter, mip_filter, color_key, source_info, palette, texture);
    }

    ReentrancyScope scope;
    const void* selected_data{};
    std::size_t selected_size{};
    std::shared_ptr<const ByteStorage> replacement_lifetime;
    const bool replaced = SelectReplacement(
        source_data,
        static_cast<std::size_t>(source_size),
        selected_data,
        selected_size,
        replacement_lifetime);
    if (!replaced) {
        g_passthrough_calls.fetch_add(1, std::memory_order_relaxed);
    }

    const HRESULT call_result = original(
        device,
        selected_data,
        static_cast<UINT>(selected_size),
        width,
        height,
        mip_levels,
        usage,
        format,
        pool,
        filter,
        mip_filter,
        color_key,
        source_info,
        palette,
        texture);
    if (replaced && SUCCEEDED(call_result)) {
        g_replacements_applied.fetch_add(1, std::memory_order_relaxed);
        NotifyOnce(TextureHookEvent::replacement_applied, g_reported_replacement);
    }
    return call_result;
}

enum class HookPreparation {
    unavailable,
    already_active,
    ready,
    failed,
};

void RecordMinHookStatus(const MH_STATUS status) noexcept {
    g_last_minhook_status.store(
        static_cast<int>(status), std::memory_order_release);
}

template <typename Function>
[[nodiscard]] HookPreparation PrepareExportHook(
    const wchar_t* module_name,
    const char* export_name,
    void* detour,
    std::atomic<Function>& original,
    void*& target,
    bool& created,
    std::atomic<bool>& installed,
    bool& export_was_present,
    bool& created_now) noexcept {
    created_now = false;
    if (installed.load(std::memory_order_acquire)) {
        export_was_present = true;
        return HookPreparation::already_active;
    }
    if (created) {
        export_was_present = true;
        if (target == nullptr ||
            original.load(std::memory_order_acquire) == nullptr) {
            return HookPreparation::failed;
        }
        return HookPreparation::ready;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            module_name,
            &module)) {
        return HookPreparation::unavailable;
    }

    const FARPROC exported = GetProcAddress(module, export_name);
    if (exported == nullptr) {
        return HookPreparation::unavailable;
    }
    export_was_present = true;
    void* const resolved_target = reinterpret_cast<void*>(exported);

    void* original_address = nullptr;
    const MH_STATUS create_status = MH_CreateHook(
        resolved_target, detour, &original_address);
    RecordMinHookStatus(create_status);
    if (create_status != MH_OK) {
        return HookPreparation::failed;
    }

    target = resolved_target;
    original.store(
        reinterpret_cast<Function>(original_address),
        std::memory_order_release);
    created = true;
    created_now = true;
    return HookPreparation::ready;
}

template <typename Function>
[[nodiscard]] bool RemoveNeverAppliedHook(
    const bool created_now,
    std::atomic<Function>& original,
    void*& target,
    bool& created,
    std::atomic<bool>& installed) noexcept {
    if (!created_now) {
        return true;
    }

    const MH_STATUS remove_status = MH_RemoveHook(target);
    if (remove_status != MH_OK && remove_status != MH_ERROR_NOT_CREATED) {
        RecordMinHookStatus(remove_status);
        return false;
    }

    installed.store(false, std::memory_order_release);
    created = false;
    original.store(nullptr, std::memory_order_release);
    target = nullptr;
    return true;
}

void ResetQueuedEnable(const HookPreparation preparation, void* const target) noexcept {
    if (preparation == HookPreparation::ready && target != nullptr) {
        static_cast<void>(MH_QueueDisableHook(target));
    }
}

[[nodiscard]] bool QueuePreparedHook(
    const HookPreparation preparation,
    void* const target,
    MH_STATUS& first_failure) noexcept {
    if (preparation != HookPreparation::ready) {
        return true;
    }

    const MH_STATUS queue_status = MH_QueueEnableHook(target);
    if (queue_status != MH_OK) {
        if (first_failure == MH_OK) {
            first_failure = queue_status;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool RollBackAfterApplyFailure(
    const HookPreparation d3dx11_preparation,
    const HookPreparation d3dx9_preparation) noexcept {
    MH_STATUS rollback_failure = MH_OK;
    const auto queue_disable = [&rollback_failure](
                                   const HookPreparation preparation,
                                   void* const target) noexcept {
        if (preparation != HookPreparation::ready) {
            return;
        }
        const MH_STATUS status = MH_QueueDisableHook(target);
        if (status != MH_OK && rollback_failure == MH_OK) {
            rollback_failure = status;
        }
    };

    queue_disable(d3dx11_preparation, g_d3dx11_target);
    queue_disable(d3dx9_preparation, g_d3dx9_target);

    // MH_ApplyQueued can fail after enabling only part of the cohort. Never
    // remove hooks past this point: another thread may already be executing a
    // detour. Queue a compensating disable operation, apply it as one batch,
    // and retain every trampoline and its backing storage for process life.
    const MH_STATUS rollback_apply_status = MH_ApplyQueued();
    if (rollback_apply_status != MH_OK && rollback_failure == MH_OK) {
        rollback_failure = rollback_apply_status;
    }

    if (rollback_failure != MH_OK) {
        RecordMinHookStatus(rollback_failure);
        return false;
    }

    if (d3dx11_preparation == HookPreparation::ready) {
        g_d3dx11_installed.store(false, std::memory_order_release);
    }
    if (d3dx9_preparation == HookPreparation::ready) {
        g_d3dx9_installed.store(false, std::memory_order_release);
    }
    return true;
}

[[nodiscard]] bool DisableHook(
    void* const target,
    const bool created,
    std::atomic<bool>& installed) noexcept {
    if (target != nullptr && created) {
        const MH_STATUS disable_status = MH_DisableHook(target);
        if (disable_status != MH_OK && disable_status != MH_ERROR_DISABLED) {
            RecordMinHookStatus(disable_status);
            return false;
        }
    }
    installed.store(false, std::memory_order_release);
    return true;
}

}  // namespace

void SetTextureHookEventCallback(const TextureHookEventCallback callback) noexcept {
    g_event_callback.store(callback, std::memory_order_release);
}

bool InitializeFirstTextureOverride(
    std::shared_ptr<const ByteStorage> replacement) noexcept {
    std::scoped_lock lock{g_lifecycle_mutex};
    const auto current_state = g_state.load(std::memory_order_acquire);
    if (current_state == TextureHookState::stopped ||
        current_state == TextureHookState::indeterminate) {
        return false;
    }
    if (!ValidateReplacement(replacement)) {
        if (current_state != TextureHookState::active &&
            current_state != TextureHookState::stopped) {
            g_replacement.store(nullptr, std::memory_order_release);
            g_state.store(
                TextureHookState::invalid_replacement,
                std::memory_order_release);
        }
        return false;
    }

    g_replacement.store(std::move(replacement), std::memory_order_release);

    const MH_STATUS initialize_status = MH_Initialize();
    g_last_minhook_status.store(
        static_cast<int>(initialize_status), std::memory_order_release);
    if (initialize_status != MH_OK &&
        initialize_status != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(TextureHookState::installation_failed, std::memory_order_release);
        g_installation_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool d3dx11_export_present = false;
    bool d3dx9_export_present = false;
    bool d3dx11_created_now = false;
    bool d3dx9_created_now = false;
    const HookPreparation d3dx11_preparation = PrepareExportHook(
        L"d3dx11_43.dll",
        "D3DX11CreateTextureFromMemory",
        reinterpret_cast<void*>(&HookD3dx11CreateTextureFromMemory),
        g_original_d3dx11,
        g_d3dx11_target,
        g_d3dx11_created,
        g_d3dx11_installed,
        d3dx11_export_present,
        d3dx11_created_now);
    const HookPreparation d3dx9_preparation = PrepareExportHook(
        L"d3dx9_43.dll",
        "D3DXCreateTextureFromFileInMemoryEx",
        reinterpret_cast<void*>(&HookD3dx9CreateTextureFromFileInMemoryEx),
        g_original_d3dx9,
        g_d3dx9_target,
        g_d3dx9_created,
        g_d3dx9_installed,
        d3dx9_export_present,
        d3dx9_created_now);

    const auto finish_failure = []() noexcept {
        g_installation_failures.fetch_add(1, std::memory_order_relaxed);
        const bool any_active =
            g_d3dx11_installed.load(std::memory_order_acquire) ||
            g_d3dx9_installed.load(std::memory_order_acquire);
        g_state.store(
            any_active ? TextureHookState::active
                       : TextureHookState::installation_failed,
            std::memory_order_release);
        return any_active;
    };

    if (d3dx11_preparation == HookPreparation::failed ||
        d3dx9_preparation == HookPreparation::failed) {
        static_cast<void>(RemoveNeverAppliedHook(
            d3dx11_created_now,
            g_original_d3dx11,
            g_d3dx11_target,
            g_d3dx11_created,
            g_d3dx11_installed));
        static_cast<void>(RemoveNeverAppliedHook(
            d3dx9_created_now,
            g_original_d3dx9,
            g_d3dx9_target,
            g_d3dx9_created,
            g_d3dx9_installed));
        return finish_failure();
    }

    const bool has_prepared_hooks =
        d3dx11_preparation == HookPreparation::ready ||
        d3dx9_preparation == HookPreparation::ready;
    if (has_prepared_hooks) {
        MH_STATUS queue_failure = MH_OK;
        const bool d3dx11_queued = QueuePreparedHook(
            d3dx11_preparation, g_d3dx11_target, queue_failure);
        const bool d3dx9_queued = QueuePreparedHook(
            d3dx9_preparation, g_d3dx9_target, queue_failure);
        if (!d3dx11_queued || !d3dx9_queued) {
            ResetQueuedEnable(d3dx11_preparation, g_d3dx11_target);
            ResetQueuedEnable(d3dx9_preparation, g_d3dx9_target);
            static_cast<void>(RemoveNeverAppliedHook(
                d3dx11_created_now,
                g_original_d3dx11,
                g_d3dx11_target,
                g_d3dx11_created,
                g_d3dx11_installed));
            static_cast<void>(RemoveNeverAppliedHook(
                d3dx9_created_now,
                g_original_d3dx9,
                g_d3dx9_target,
                g_d3dx9_created,
                g_d3dx9_installed));
            RecordMinHookStatus(queue_failure);
            return finish_failure();
        }

        // Publish both trampolines before this single activation point. An
        // acquire load in each detour then observes a fully initialized slot.
        const MH_STATUS apply_status = MH_ApplyQueued();
        RecordMinHookStatus(apply_status);
        if (apply_status != MH_OK) {
            if (!RollBackAfterApplyFailure(
                    d3dx11_preparation, d3dx9_preparation)) {
                g_replacement.store(nullptr, std::memory_order_release);
                g_state.store(
                    TextureHookState::indeterminate,
                    std::memory_order_release);
                g_installation_failures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            return finish_failure();
        }

        if (d3dx11_preparation == HookPreparation::ready) {
            g_d3dx11_installed.store(true, std::memory_order_release);
        }
        if (d3dx9_preparation == HookPreparation::ready) {
            g_d3dx9_installed.store(true, std::memory_order_release);
        }
    }

    const bool any_active =
        g_d3dx11_installed.load(std::memory_order_acquire) ||
        g_d3dx9_installed.load(std::memory_order_acquire);
    if (any_active) {
        g_state.store(TextureHookState::active, std::memory_order_release);
        return true;
    }

    const bool any_export_present =
        d3dx11_export_present || d3dx9_export_present;
    g_state.store(
        any_export_present ? TextureHookState::installation_failed
                           : TextureHookState::waiting_for_d3dx,
        std::memory_order_release);
    return false;
}

TextureHookStatus GetTextureHookStatus() noexcept {
    return TextureHookStatus{
        g_state.load(std::memory_order_acquire),
        g_d3dx11_installed.load(std::memory_order_acquire),
        g_d3dx9_installed.load(std::memory_order_acquire),
        g_last_minhook_status.load(std::memory_order_acquire),
    };
}

TextureHookStats GetTextureHookStats() noexcept {
    return TextureHookStats{
        g_d3dx11_calls.load(std::memory_order_relaxed),
        g_d3dx9_calls.load(std::memory_order_relaxed),
        g_size_candidates.load(std::memory_order_relaxed),
        g_original_hash_matches.load(std::memory_order_relaxed),
        g_replacements_applied.load(std::memory_order_relaxed),
        g_hash_failures.load(std::memory_order_relaxed),
        g_reentrant_bypasses.load(std::memory_order_relaxed),
        g_asynchronous_bypasses.load(std::memory_order_relaxed),
        g_passthrough_calls.load(std::memory_order_relaxed),
        g_installation_failures.load(std::memory_order_relaxed),
    };
}

void ShutdownTextureHooks() noexcept {
    std::scoped_lock lock{g_lifecycle_mutex};
    const bool d3dx11_disabled =
        DisableHook(g_d3dx11_target, g_d3dx11_created, g_d3dx11_installed);
    const bool d3dx9_disabled =
        DisableHook(g_d3dx9_target, g_d3dx9_created, g_d3dx9_installed);
    if (d3dx11_disabled && d3dx9_disabled) {
        g_state.store(TextureHookState::stopped, std::memory_order_release);
    } else {
        g_replacement.store(nullptr, std::memory_order_release);
        g_state.store(TextureHookState::indeterminate, std::memory_order_release);
    }
}

}  // namespace ds2::modding
