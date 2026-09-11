#include "pch.h"

#include "bootstrap.h"
#include "proxy_dinput8.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) noexcept
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        ds2::proxy::SetProxyModule(module);
        // Schedule only. Windows serializes DLL initialization, so the new
        // worker cannot enter the heavy framework path until loader startup
        // has completed; this thread is never awaited from DllMain.
        static_cast<void>(ds2::bootstrap::EnsureInitialized());
    }

    // The real DirectInput DLL, hashing, filesystem traversal, logging and
    // MinHook setup remain outside DllMain and its loader lock.
    return TRUE;
}
