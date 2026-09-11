#include "pch.h"

#include "bootstrap.h"

#include "asset_hook.h"
#include "game_build.h"
#include "logging.h"
#include "mod_index.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <ShlObj.h>

namespace
{
    constexpr std::wstring_view kTargetVirtualPath =
        L"media/ui/ui_icons_small/ui_hudicon_passiveability_improved_agility.dds";

    INIT_ONCE g_bootstrapInitOnce = INIT_ONCE_STATIC_INIT;
    std::atomic<ds2::bootstrap::Status> g_bootstrapStatus =
        ds2::bootstrap::Status::not_started;
    std::atomic<std::shared_ptr<ds2::modding::SessionLogger>> g_logger;
    std::atomic<std::shared_ptr<const ds2::modding::ModIndexSnapshot>> g_mod_index;
    std::atomic<unsigned long> g_pending_texture_events{};
    std::atomic<HANDLE> g_texture_event_handle{};

    constexpr unsigned long kD3dx11FirstEntryEvent = 1u << 0;
    constexpr unsigned long kD3dx9FirstEntryEvent = 1u << 1;
    constexpr unsigned long kOriginalMatchedEvent = 1u << 2;
    constexpr unsigned long kReplacementAppliedEvent = 1u << 3;
    constexpr unsigned int kTextureHookAttempts = 100;
    constexpr DWORD kTextureHookRetryDelayMilliseconds = 100;

    [[nodiscard]] std::optional<std::filesystem::path> GetLogDirectory()
    {
        PWSTR raw_path = nullptr;
        const HRESULT result = SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw_path);
        if (FAILED(result) || raw_path == nullptr)
        {
            return std::nullopt;
        }

        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(
            raw_path, CoTaskMemFree);
        std::filesystem::path path(owned_path.get());
        return path / L"Darksiders2DLL" / L"logs";
    }

    void LogEvent(
        const std::wstring_view event_name,
        const std::wstring_view detail = {}) noexcept
    {
        const auto logger = g_logger.load(std::memory_order_acquire);
        if (logger != nullptr)
        {
            static_cast<void>(logger->Event(event_name, detail));
        }
    }

    [[nodiscard]] const wchar_t* BoolName(const bool value) noexcept
    {
        return value ? L"true" : L"false";
    }

    DWORD WINAPI TextureEventLogThread(LPVOID) noexcept
    {
        const HANDLE event_handle = g_texture_event_handle.load(std::memory_order_acquire);
        if (event_handle == nullptr)
        {
            return ERROR_INVALID_HANDLE;
        }

        for (;;)
        {
            const DWORD wait_result = WaitForSingleObject(event_handle, INFINITE);
            if (wait_result != WAIT_OBJECT_0)
            {
                return GetLastError();
            }

            const unsigned long events = g_pending_texture_events.exchange(
                0, std::memory_order_acq_rel);
            if ((events & kD3dx11FirstEntryEvent) != 0)
            {
                LogEvent(
                    L"D3DX11_FIRST_ENTRY",
                    L"D3DX11CreateTextureFromMemory");
                OutputDebugStringW(L"[Darksiders2DLL] D3DX11_FIRST_ENTRY\n");
            }
            if ((events & kD3dx9FirstEntryEvent) != 0)
            {
                LogEvent(
                    L"D3DX9_FIRST_ENTRY",
                    L"D3DXCreateTextureFromFileInMemoryEx");
                OutputDebugStringW(L"[Darksiders2DLL] D3DX9_FIRST_ENTRY\n");
            }
            if ((events & kOriginalMatchedEvent) != 0)
            {
                LogEvent(L"ASSET_REQUEST", kTargetVirtualPath);
                OutputDebugStringW(L"[Darksiders2DLL] ASSET_REQUEST target DDS matched\n");
            }
            if ((events & kReplacementAppliedEvent) != 0)
            {
                LogEvent(
                    L"ASSET_OVERRIDE_HIT",
                    L"first_test_mod media/ui/ui_icons_small/"
                    L"ui_hudicon_passiveability_improved_agility.dds "
                    L"size=4224 sha256=2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719");
                OutputDebugStringW(L"[Darksiders2DLL] ASSET_OVERRIDE_HIT first_test_mod\n");
            }
        }
    }

    [[nodiscard]] bool StartTextureEventLogger() noexcept
    {
        const HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_handle == nullptr)
        {
            return false;
        }
        g_texture_event_handle.store(event_handle, std::memory_order_release);

        const HANDLE thread = CreateThread(
            nullptr, 0, TextureEventLogThread, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            g_texture_event_handle.store(nullptr, std::memory_order_release);
            CloseHandle(event_handle);
            return false;
        }
        CloseHandle(thread);
        return true;
    }

    void OnTextureHookEvent(const ds2::modding::TextureHookEvent event) noexcept
    {
        unsigned long event_flag = 0;
        switch (event)
        {
        case ds2::modding::TextureHookEvent::d3dx11_first_entry:
            event_flag = kD3dx11FirstEntryEvent;
            break;
        case ds2::modding::TextureHookEvent::d3dx9_first_entry:
            event_flag = kD3dx9FirstEntryEvent;
            break;
        case ds2::modding::TextureHookEvent::original_asset_matched:
            event_flag = kOriginalMatchedEvent;
            break;
        case ds2::modding::TextureHookEvent::replacement_applied:
            event_flag = kReplacementAppliedEvent;
            break;
        default:
            return;
        }
        g_pending_texture_events.fetch_or(event_flag, std::memory_order_release);
        const HANDLE event_handle = g_texture_event_handle.load(std::memory_order_acquire);
        if (event_handle != nullptr)
        {
            SetEvent(event_handle);
        }
    }

    [[nodiscard]] bool PinThisModule() noexcept
    {
        HMODULE pinned_module = nullptr;
        return GetModuleHandleExW(
                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_PIN,
                   reinterpret_cast<LPCWSTR>(std::addressof(g_bootstrapStatus)),
                   &pinned_module) != FALSE;
    }

    void RunFramework() noexcept
    {
        try
        {
            if (!PinThisModule())
            {
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] MODULE_PIN_FAILED\n");
                return;
            }

            const auto log_directory = GetLogDirectory();
            if (!log_directory)
            {
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::logging_initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] LOGGING_INITIALIZATION_FAILED\n");
                return;
            }

            auto logger_result = ds2::modding::OpenSessionLogger(*log_directory);
            if (!logger_result)
            {
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::logging_initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] LOGGING_INITIALIZATION_FAILED\n");
                return;
            }
            g_logger.store(std::move(logger_result.logger), std::memory_order_release);
            LogEvent(L"SESSION_START", L"version=0.1.0 mode=d3dx_texture_poc");

            const auto build = ds2::modding::IdentifyCurrentGameBuild();
            std::wstring build_detail = L"path=";
            build_detail.append(build.executable_path.native());
            build_detail.append(L" size=");
            build_detail.append(std::to_wstring(build.file_size));
            if (!build.sha256_hex.empty())
            {
                build_detail.append(L" sha256=");
                build_detail.append(build.sha256_hex);
            }
            if (!build.Supported())
            {
                build_detail.append(L" reason=");
                build_detail.append(ds2::modding::GameBuildErrorName(build.error));
                LogEvent(L"BUILD_UNSUPPORTED", build_detail);
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::unsupported_build,
                    std::memory_order_release);
                return;
            }

            LogEvent(L"BUILD_SUPPORTED", build_detail);
            const auto game_directory = build.executable_path.parent_path();
            auto index_result = ds2::modding::BuildModIndex(game_directory / L"mods");
            if (!index_result)
            {
                std::wstring detail = L"reason=";
                detail.append(ds2::modding::ModIndexErrorName(index_result.error));
                detail.append(L" win32=");
                detail.append(std::to_wstring(index_result.system_error));
                LogEvent(L"MOD_INDEX_FAILED", detail);
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::mod_index_initialization_failed,
                    std::memory_order_release);
                return;
            }

            for (const auto& mod_id : index_result.snapshot->ModIds())
            {
                LogEvent(L"MOD_INDEXED", mod_id);
            }
            for (const auto& issue : index_result.issues)
            {
                std::wstring detail = L"code=";
                detail.append(ds2::modding::ModIndexIssueCodeName(issue.code));
                if (!issue.mod_id.empty())
                {
                    detail.append(L" mod=");
                    detail.append(issue.mod_id);
                }
                detail.append(L" path=");
                detail.append(issue.path.native());
                LogEvent(L"MOD_INDEX_ISSUE", detail);
            }

            auto snapshot = std::move(index_result.snapshot);
            const auto* const asset = snapshot->Find(kTargetVirtualPath);
            g_mod_index.store(snapshot, std::memory_order_release);
            LogEvent(
                L"GENERAL_RESOLVER_DISABLED",
                L"dynamic ABI and ownership validation still required; texture POC only");

            if (asset == nullptr)
            {
                LogEvent(L"ASSET_FALLBACK", L"target DDS is absent from the mod index");
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::ready_proxy_only,
                    std::memory_order_release);
                return;
            }
            if (asset->mod_id != L"first_test_mod")
            {
                LogEvent(L"ASSET_FALLBACK", L"target winner is not first_test_mod");
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::ready_proxy_only,
                    std::memory_order_release);
                return;
            }

            std::wstring ready_detail = L"mod=";
            ready_detail.append(asset->mod_id);
            ready_detail.append(L" path=");
            ready_detail.append(asset->virtual_path.display);
            ready_detail.append(L" size=");
            ready_detail.append(std::to_wstring(asset->storage->Size()));
            ready_detail.append(L" sha256=");
            ready_detail.append(asset->storage->Sha256Hex());
            LogEvent(L"ASSET_OVERRIDE_READY", ready_detail);

            if (!StartTextureEventLogger())
            {
                LogEvent(L"TEXTURE_EVENT_LOGGER_FAILED");
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::texture_hook_initialization_failed,
                    std::memory_order_release);
                return;
            }
            ds2::modding::SetTextureHookEventCallback(OnTextureHookEvent);
            bool texture_hook_active = false;
            ds2::modding::TextureHookStatus hook_status;
            for (unsigned int attempt = 0; attempt < kTextureHookAttempts; ++attempt)
            {
                if (ds2::modding::InitializeFirstTextureOverride(asset->storage))
                {
                    texture_hook_active = true;
                    break;
                }
                hook_status = ds2::modding::GetTextureHookStatus();
                if (hook_status.state != ds2::modding::TextureHookState::waiting_for_d3dx)
                {
                    break;
                }
                if (attempt + 1 < kTextureHookAttempts)
                {
                    Sleep(kTextureHookRetryDelayMilliseconds);
                }
            }
            if (!texture_hook_active)
            {
                std::wstring detail = L"minhook_status=";
                detail.append(std::to_wstring(hook_status.last_minhook_status));
                if (hook_status.state == ds2::modding::TextureHookState::waiting_for_d3dx)
                {
                    LogEvent(L"TEXTURE_HOOK_UNAVAILABLE", detail);
                    g_bootstrapStatus.store(
                        ds2::bootstrap::Status::ready_proxy_only,
                        std::memory_order_release);
                }
                else
                {
                    LogEvent(L"TEXTURE_HOOK_FAILED", detail);
                    g_bootstrapStatus.store(
                        ds2::bootstrap::Status::texture_hook_initialization_failed,
                        std::memory_order_release);
                }
                return;
            }

            hook_status = ds2::modding::GetTextureHookStatus();
            std::wstring hook_detail = L"d3dx11=";
            hook_detail.append(BoolName(hook_status.d3dx11_hook_installed));
            hook_detail.append(L" d3dx9=");
            hook_detail.append(BoolName(hook_status.d3dx9_hook_installed));
            LogEvent(L"TEXTURE_HOOK_ACTIVE", hook_detail);
            g_bootstrapStatus.store(
                ds2::bootstrap::Status::ready_texture_override,
                std::memory_order_release);
            OutputDebugStringW(L"[Darksiders2DLL] TEXTURE_HOOK_ACTIVE\n");
        }
        catch (...)
        {
            if (ds2::modding::GetTextureHookStatus().state ==
                ds2::modding::TextureHookState::active)
            {
                LogEvent(
                    L"INITIALIZATION_WARNING",
                    L"unhandled C++ exception after texture hook activation");
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::ready_texture_override,
                    std::memory_order_release);
                OutputDebugStringW(
                    L"[Darksiders2DLL] INITIALIZATION_WARNING_AFTER_TEXTURE_HOOK_ACTIVE\n");
            }
            else
            {
                LogEvent(L"INITIALIZATION_FAILED", L"unhandled C++ exception");
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] INITIALIZATION_FAILED\n");
            }
        }
    }

    DWORD WINAPI BootstrapThread(LPVOID) noexcept
    {
        RunFramework();
        return 0;
    }

    BOOL CALLBACK ScheduleFramework(
        PINIT_ONCE initOnce,
        PVOID parameter,
        PVOID* context) noexcept
    {
        UNREFERENCED_PARAMETER(initOnce);
        UNREFERENCED_PARAMETER(parameter);
        UNREFERENCED_PARAMETER(context);

        g_bootstrapStatus.store(ds2::bootstrap::Status::starting, std::memory_order_release);
        const HANDLE thread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            g_bootstrapStatus.store(
                ds2::bootstrap::Status::initialization_failed,
                std::memory_order_release);
            OutputDebugStringW(L"[Darksiders2DLL] BOOTSTRAP_THREAD_FAILED\n");
            // Leave INIT_ONCE incomplete so a later DirectInput8Create call
            // can retry scheduling after a transient thread-creation failure.
            return FALSE;
        }
        CloseHandle(thread);
        return TRUE;
    }
}

namespace ds2::bootstrap
{
    Status EnsureInitialized() noexcept
    {
        static_cast<void>(InitOnceExecuteOnce(
            &g_bootstrapInitOnce,
            ScheduleFramework,
            nullptr,
            nullptr));

        return CurrentStatus();
    }

    Status CurrentStatus() noexcept
    {
        return g_bootstrapStatus.load(std::memory_order_acquire);
    }

    const wchar_t* StatusName(const Status status) noexcept
    {
        switch (status)
        {
        case Status::not_started: return L"not_started";
        case Status::starting: return L"starting";
        case Status::ready_proxy_only: return L"ready_proxy_only";
        case Status::ready_texture_override: return L"ready_texture_override";
        case Status::unsupported_build: return L"unsupported_build";
        case Status::logging_initialization_failed: return L"logging_initialization_failed";
        case Status::mod_index_initialization_failed: return L"mod_index_initialization_failed";
        case Status::texture_hook_initialization_failed: return L"texture_hook_initialization_failed";
        case Status::initialization_failed: return L"initialization_failed";
        }
        return L"unknown";
    }
}
