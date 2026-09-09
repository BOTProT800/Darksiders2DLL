#include "pch.h"

#include "bootstrap.h"

#include <MinHook.h>

#include <atomic>

namespace
{
    INIT_ONCE g_bootstrapInitOnce = INIT_ONCE_STATIC_INIT;
    std::atomic<ds2::bootstrap::Status> g_bootstrapStatus =
        ds2::bootstrap::Status::not_started;

    BOOL CALLBACK InitializeFramework(
        PINIT_ONCE initOnce,
        PVOID parameter,
        PVOID* context) noexcept
    {
        UNREFERENCED_PARAMETER(initOnce);
        UNREFERENCED_PARAMETER(parameter);
        UNREFERENCED_PARAMETER(context);

        // MinHook owns no active hooks at this stage. Initializing it here
        // validates the dependency and keeps all loader work outside DllMain.
        const MH_STATUS minHookStatus = MH_Initialize();
        if (minHookStatus != MH_OK && minHookStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            g_bootstrapStatus.store(
                ds2::bootstrap::Status::minhook_initialization_failed,
                std::memory_order_release);
            OutputDebugStringW(L"[Darksiders2DLL] MINHOOK_INITIALIZATION_FAILED\n");
            return TRUE;
        }

        // Fingerprinting, logging, mod indexing, and hook installation plug in
        // here. They must fail closed and may not make DirectInput unavailable.
        g_bootstrapStatus.store(ds2::bootstrap::Status::ready, std::memory_order_release);
        OutputDebugStringW(L"[Darksiders2DLL] BOOTSTRAP_READY\n");
        return TRUE;
    }
}

namespace ds2::bootstrap
{
    Status EnsureInitialized() noexcept
    {
        if (!InitOnceExecuteOnce(
                &g_bootstrapInitOnce,
                InitializeFramework,
                nullptr,
                nullptr))
        {
            g_bootstrapStatus.store(Status::initialization_failed, std::memory_order_release);
        }

        return CurrentStatus();
    }

    Status CurrentStatus() noexcept
    {
        return g_bootstrapStatus.load(std::memory_order_acquire);
    }
}
