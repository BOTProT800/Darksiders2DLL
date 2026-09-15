#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <Windows.h>
#include <dinput.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using DirectInput8CreateFunction = HRESULT(WINAPI*)(
    HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using GetdfDIJoystickFunction = LPCDIDATAFORMAT(WINAPI*)();

template <typename Function>
Function Resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

bool SamePath(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code error;
    const auto normalized_left = std::filesystem::weakly_canonical(left, error);
    if (error) {
        return false;
    }
    const auto normalized_right = std::filesystem::weakly_canonical(right, error);
    return !error && _wcsicmp(
        normalized_left.c_str(), normalized_right.c_str()) == 0;
}

int Fail(const std::wstring_view message) {
    std::wcerr << L"FAIL: " << message << L'\n';
    return 1;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) {
        return Fail(L"usage: proxy_smoke.exe <absolute-path-to-dinput8.dll>");
    }

    const std::filesystem::path proxy_path(argv[1]);
    if (!proxy_path.is_absolute()) {
        return Fail(L"the proxy path must be absolute");
    }

    const HMODULE proxy = LoadLibraryExW(
        proxy_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (proxy == nullptr) {
        std::wcerr << L"FAIL: LoadLibraryExW error=" << GetLastError() << L'\n';
        return 1;
    }

    constexpr std::array<const char*, 6> export_names{
        "DirectInput8Create",
        "DllCanUnloadNow",
        "DllGetClassObject",
        "DllRegisterServer",
        "DllUnregisterServer",
        "GetdfDIJoystick",
    };
    for (std::size_t index = 0; index < export_names.size(); ++index) {
        const FARPROC by_name = GetProcAddress(proxy, export_names[index]);
        const FARPROC by_ordinal = GetProcAddress(
            proxy, MAKEINTRESOURCEA(static_cast<WORD>(index + 1)));
        if (by_name == nullptr || by_ordinal == nullptr || by_name != by_ordinal) {
            std::wcerr << L"FAIL: export/ordinal mismatch at " << index + 1 << L'\n';
            return 1;
        }
    }

    const auto get_joystick_format = Resolve<GetdfDIJoystickFunction>(
        proxy, "GetdfDIJoystick");
    if (get_joystick_format == nullptr || get_joystick_format() == nullptr) {
        return Fail(L"GetdfDIJoystick did not forward to the system DLL");
    }

    wchar_t system_directory[32768]{};
    const UINT system_length = GetSystemDirectoryW(
        system_directory, static_cast<UINT>(std::size(system_directory)));
    if (system_length == 0 || system_length >= std::size(system_directory)) {
        return Fail(L"GetSystemDirectoryW failed");
    }
    const auto system_dinput =
        std::filesystem::path(system_directory) / L"dinput8.dll";
    const HMODULE real_dinput = GetModuleHandleW(system_dinput.c_str());
    if (real_dinput == nullptr || real_dinput == proxy) {
        return Fail(L"the distinct system dinput8.dll was not loaded by absolute path");
    }

    wchar_t loaded_path[32768]{};
    const DWORD loaded_length = GetModuleFileNameW(
        real_dinput, loaded_path, static_cast<DWORD>(std::size(loaded_path)));
    if (loaded_length == 0 || loaded_length >= std::size(loaded_path) ||
        !SamePath(loaded_path, system_dinput)) {
        return Fail(L"the forwarded module is not System32\\dinput8.dll");
    }

    const auto direct_input_create = Resolve<DirectInput8CreateFunction>(
        proxy, "DirectInput8Create");
    if (direct_input_create == nullptr) {
        return Fail(L"DirectInput8Create is absent");
    }

    void* direct_input = nullptr;
    const HRESULT result = direct_input_create(
        GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        &direct_input,
        nullptr);
    if (FAILED(result) || direct_input == nullptr) {
        std::wcerr << L"FAIL: DirectInput8Create HRESULT=0x" << std::hex
                   << static_cast<unsigned long>(result) << L'\n';
        return 1;
    }

    static_cast<IUnknown*>(direct_input)->Release();
    std::wcout << L"PASS: six exports, ordinal parity, System32 forwarding, "
                  L"and DirectInput8Create verified\n";
    return 0;
}
