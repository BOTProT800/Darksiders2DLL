#include "pch.h"

#include "proxy_dinput8.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) noexcept
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        ds2::proxy::SetProxyModule(module);
        DisableThreadLibraryCalls(module);
    }

    // Loading the real dinput8.dll and starting the mod framework here would
    // run arbitrary work while the Windows loader lock is held. Both actions
    // are intentionally deferred to the exported DirectInput8Create wrapper.
    return TRUE;
}
