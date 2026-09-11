#include "pch.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "bootstrap.h"
#include "proxy_dinput8.h"

#include <atomic>
#include <cstddef>

namespace
{
    using DirectInput8CreateFunction = HRESULT(WINAPI*)(
        HINSTANCE,
        DWORD,
        REFIID,
        LPVOID*,
        LPUNKNOWN);
    using DllCanUnloadNowFunction = HRESULT(STDAPICALLTYPE*)();
    using DllGetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    using DllRegisterServerFunction = HRESULT(STDAPICALLTYPE*)();
    using DllUnregisterServerFunction = HRESULT(STDAPICALLTYPE*)();
    using GetdfDIJoystickFunction = LPCDIDATAFORMAT(WINAPI*)();

    struct SystemDinput8Exports
    {
        HMODULE module = nullptr;
        DirectInput8CreateFunction direct_input_8_create = nullptr;
        DllCanUnloadNowFunction dll_can_unload_now = nullptr;
        DllGetClassObjectFunction dll_get_class_object = nullptr;
        DllRegisterServerFunction dll_register_server = nullptr;
        DllUnregisterServerFunction dll_unregister_server = nullptr;
        GetdfDIJoystickFunction get_df_di_joystick = nullptr;
    };

    INIT_ONCE g_systemDinput8InitOnce = INIT_ONCE_STATIC_INIT;
    SystemDinput8Exports g_systemDinput8;
    std::atomic<HMODULE> g_proxyModule = nullptr;
    std::atomic<DWORD> g_systemDinput8LoadError = ERROR_SUCCESS;
    wchar_t g_systemDinput8Path[32768] = {};

    template <typename Function>
    [[nodiscard]] Function ResolveExport(HMODULE module, const char* name) noexcept
    {
#pragma warning(push)
#pragma warning(disable : 4191) // GetProcAddress is the supported Win32 conversion point.
        return reinterpret_cast<Function>(GetProcAddress(module, name));
#pragma warning(pop)
    }

    void RememberLoadFailure(DWORD error) noexcept
    {
        g_systemDinput8LoadError.store(
            error == ERROR_SUCCESS ? ERROR_DLL_INIT_FAILED : error,
            std::memory_order_release);
    }

    BOOL CALLBACK LoadSystemDinput8(
        PINIT_ONCE initOnce,
        PVOID parameter,
        PVOID* context) noexcept
    {
        UNREFERENCED_PARAMETER(initOnce);
        UNREFERENCED_PARAMETER(parameter);
        UNREFERENCED_PARAMETER(context);

        constexpr wchar_t kDinput8FileName[] = L"dinput8.dll";

        const UINT directoryLength = GetSystemDirectoryW(
            g_systemDinput8Path,
            static_cast<UINT>(ARRAYSIZE(g_systemDinput8Path)));
        if (directoryLength == 0 || directoryLength >= ARRAYSIZE(g_systemDinput8Path))
        {
            RememberLoadFailure(directoryLength == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
            return TRUE; // Cache the failure; repeated export calls must not spin on loading.
        }

        std::size_t pathLength = directoryLength;
        if (pathLength != 0 &&
            g_systemDinput8Path[pathLength - 1] != L'\\' &&
            g_systemDinput8Path[pathLength - 1] != L'/')
        {
            if (pathLength + 1 >= ARRAYSIZE(g_systemDinput8Path))
            {
                RememberLoadFailure(ERROR_INSUFFICIENT_BUFFER);
                return TRUE;
            }
            g_systemDinput8Path[pathLength++] = L'\\';
        }

        if (pathLength + ARRAYSIZE(kDinput8FileName) > ARRAYSIZE(g_systemDinput8Path))
        {
            RememberLoadFailure(ERROR_INSUFFICIENT_BUFFER);
            return TRUE;
        }
        CopyMemory(
            g_systemDinput8Path + pathLength,
            kDinput8FileName,
            sizeof(kDinput8FileName));

        // The path is absolute and rooted in the directory returned by Windows.
        // LOAD_LIBRARY_SEARCH_SYSTEM32 additionally prevents dependency lookup in
        // the game directory.
        HMODULE systemModule = LoadLibraryExW(
            g_systemDinput8Path,
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (systemModule == nullptr)
        {
            RememberLoadFailure(GetLastError());
            return TRUE;
        }

        SystemDinput8Exports exports;
        exports.module = systemModule;
        exports.direct_input_8_create = ResolveExport<DirectInput8CreateFunction>(
            systemModule,
            "DirectInput8Create");
        exports.dll_can_unload_now = ResolveExport<DllCanUnloadNowFunction>(
            systemModule,
            "DllCanUnloadNow");
        exports.dll_get_class_object = ResolveExport<DllGetClassObjectFunction>(
            systemModule,
            "DllGetClassObject");
        exports.dll_register_server = ResolveExport<DllRegisterServerFunction>(
            systemModule,
            "DllRegisterServer");
        exports.dll_unregister_server = ResolveExport<DllUnregisterServerFunction>(
            systemModule,
            "DllUnregisterServer");
        exports.get_df_di_joystick = ResolveExport<GetdfDIJoystickFunction>(
            systemModule,
            "GetdfDIJoystick");

        if (exports.direct_input_8_create == nullptr ||
            exports.dll_can_unload_now == nullptr ||
            exports.dll_get_class_object == nullptr ||
            exports.dll_register_server == nullptr ||
            exports.dll_unregister_server == nullptr ||
            exports.get_df_di_joystick == nullptr)
        {
            RememberLoadFailure(ERROR_PROC_NOT_FOUND);
            FreeLibrary(systemModule);
            g_systemDinput8Path[0] = L'\0';
            return TRUE;
        }

        g_systemDinput8 = exports;
        g_systemDinput8LoadError.store(ERROR_SUCCESS, std::memory_order_release);
        return TRUE;
    }

    [[nodiscard]] bool EnsureSystemDinput8Loaded() noexcept
    {
        if (!InitOnceExecuteOnce(
                &g_systemDinput8InitOnce,
                LoadSystemDinput8,
                nullptr,
                nullptr))
        {
            RememberLoadFailure(GetLastError());
            return false;
        }

        return g_systemDinput8.module != nullptr;
    }

    [[nodiscard]] HRESULT SystemDinput8FailureResult() noexcept
    {
        const DWORD error = g_systemDinput8LoadError.load(std::memory_order_acquire);
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_DLL_INIT_FAILED : error);
    }
}

namespace ds2::proxy
{
    void SetProxyModule(HMODULE module) noexcept
    {
        g_proxyModule.store(module, std::memory_order_release);
    }

    HMODULE ProxyModule() noexcept
    {
        return g_proxyModule.load(std::memory_order_acquire);
    }

    HMODULE SystemDinput8Module() noexcept
    {
        return EnsureSystemDinput8Loaded() ? g_systemDinput8.module : nullptr;
    }

    DWORD SystemDinput8LoadError() noexcept
    {
        return g_systemDinput8LoadError.load(std::memory_order_acquire);
    }

    const wchar_t* SystemDinput8Path() noexcept
    {
        return EnsureSystemDinput8Loaded() ? g_systemDinput8Path : L"";
    }
}

extern "C" HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    REFIID interfaceId,
    LPVOID* output,
    LPUNKNOWN outerUnknown)
{
    if (!EnsureSystemDinput8Loaded())
    {
        return SystemDinput8FailureResult();
    }

    // Do not assume the caller is outside the loader lock: the bootstrap entry
    // point only schedules a worker and never waits for framework setup.
    // Initialization remains intentionally non-fatal to DirectInput forwarding.
    static_cast<void>(ds2::bootstrap::EnsureInitialized());

    return g_systemDinput8.direct_input_8_create(
        instance,
        version,
        interfaceId,
        output,
        outerUnknown);
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    return EnsureSystemDinput8Loaded()
        ? g_systemDinput8.dll_can_unload_now()
        : SystemDinput8FailureResult();
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(
    REFCLSID classId,
    REFIID interfaceId,
    LPVOID* output)
{
    return EnsureSystemDinput8Loaded()
        ? g_systemDinput8.dll_get_class_object(classId, interfaceId, output)
        : SystemDinput8FailureResult();
}

extern "C" HRESULT STDAPICALLTYPE DllRegisterServer()
{
    return EnsureSystemDinput8Loaded()
        ? g_systemDinput8.dll_register_server()
        : SystemDinput8FailureResult();
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer()
{
    return EnsureSystemDinput8Loaded()
        ? g_systemDinput8.dll_unregister_server()
        : SystemDinput8FailureResult();
}

extern "C" LPCDIDATAFORMAT WINAPI GetdfDIJoystick()
{
    return EnsureSystemDinput8Loaded()
        ? g_systemDinput8.get_df_di_joystick()
        : nullptr;
}
