#pragma once

#include <Windows.h>

namespace ds2::proxy
{
    // Called only by DllMain during DLL_PROCESS_ATTACH. This function stores a
    // handle; it never loads DLLs, allocates memory, starts threads, or waits.
    void SetProxyModule(HMODULE module) noexcept;

    [[nodiscard]] HMODULE ProxyModule() noexcept;
    [[nodiscard]] HMODULE SystemDinput8Module() noexcept;
    [[nodiscard]] DWORD SystemDinput8LoadError() noexcept;
    [[nodiscard]] const wchar_t* SystemDinput8Path() noexcept;
}
