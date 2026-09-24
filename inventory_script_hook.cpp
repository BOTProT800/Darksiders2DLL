#include "inventory_script_hook.h"
#include <MinHook.h>
#include <atomic>
#include <cstring>

namespace ds2::modding {
namespace {
using CreateWide = decltype(&CreateFileW);
using CreateAnsi = decltype(&CreateFileA);
std::atomic<CreateWide> g_create_w{};
std::atomic<CreateAnsi> g_create_a{};
std::shared_ptr<const InventoryScriptCandidate> g_inventory_owner;
std::atomic<const InventoryScriptCandidate*> g_inventory_candidate{};
std::atomic<bool> g_inventory_active{}, g_inventory_write{}, g_inventory_started{};
std::atomic<int> g_inventory_mh{};
std::atomic<std::uint64_t> g_inventory_matched{}, g_inventory_redirected{},
    g_inventory_observed{}, g_inventory_failures{};

bool SameFile(const HANDLE file, const FILE_ID_INFO& expected) noexcept {
    FILE_ID_INFO actual{};
    return GetFileInformationByHandleEx(file, FileIdInfo, &actual, sizeof(actual)) &&
        actual.VolumeSerialNumber == expected.VolumeSerialNumber &&
        std::memcmp(actual.FileId.Identifier, expected.FileId.Identifier, sizeof(actual.FileId.Identifier)) == 0;
}
bool EligibleOpen(DWORD access, DWORD disposition, DWORD flags, HANDLE templ) noexcept {
    constexpr DWORD allowed = GENERIC_READ | FILE_READ_DATA | FILE_READ_EA |
        FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
    return (access & (GENERIC_READ | FILE_READ_DATA)) != 0 && (access & ~allowed) == 0 &&
        disposition == OPEN_EXISTING && templ == nullptr &&
        (flags & (FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT |
                  FILE_FLAG_BACKUP_SEMANTICS)) == 0;
}
// The original open runs first. Matching the authenticated file ID handles
// relative paths, case, Unicode, aliases and unrelated scripts.obsp files.
// Opening a real replacement handle preserves normal/overlapped ReadFile,
// ReadFileEx, seeking, mapping and close semantics without buffer patching.
HANDLE RedirectOpenedScript(HANDLE file, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                           DWORD disposition, DWORD flags, HANDLE templ) noexcept {
    if (file == INVALID_HANDLE_VALUE || !g_inventory_active.load(std::memory_order_acquire) ||
        !EligibleOpen(access, disposition, flags, templ)) return file;
    const auto* candidate = g_inventory_candidate.load(std::memory_order_acquire);
    if (!candidate || !SameFile(file, candidate->OriginalId())) return file;
    g_inventory_matched.fetch_add(1, std::memory_order_relaxed);
    if (!g_inventory_write.load(std::memory_order_relaxed)) {
        g_inventory_observed.fetch_add(1, std::memory_order_relaxed); return file;
    }
    // Unbuffered readers rely on the original volume's alignment/sector size.
    // Keeping both files on that volume preserves the caller's IO contract.
    if ((flags & FILE_FLAG_NO_BUFFERING) &&
        candidate->OriginalId().VolumeSerialNumber != candidate->ReplacementId().VolumeSerialNumber) {
        g_inventory_failures.fetch_add(1, std::memory_order_relaxed); return file;
    }
    const auto create = g_create_w.load(std::memory_order_acquire);
    HANDLE replacement = create(candidate->ReplacementPath().c_str(), access, share, security,
                                disposition, flags, templ);
    if (replacement == INVALID_HANDLE_VALUE) {
        g_inventory_failures.fetch_add(1, std::memory_order_relaxed); return file;
    }
    // A parent-directory rename must not redirect to a different mod file.
    if (!SameFile(replacement, candidate->ReplacementId()) || !CloseHandle(file)) {
        CloseHandle(replacement);
        g_inventory_failures.fetch_add(1, std::memory_order_relaxed); return file;
    }
    g_inventory_redirected.fetch_add(1, std::memory_order_relaxed);
    return replacement;
}
HANDLE WINAPI HookInventoryCreateW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                   DWORD disposition, DWORD flags, HANDLE templ) noexcept {
    const auto create = g_create_w.load(std::memory_order_acquire);
    HANDLE file = create(path, access, share, security, disposition, flags, templ);
    const DWORD error = GetLastError();
    file = RedirectOpenedScript(file, access, share, security, disposition, flags, templ);
    SetLastError(error);
    return file;
}
HANDLE WINAPI HookInventoryCreateA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                   DWORD disposition, DWORD flags, HANDLE templ) noexcept {
    const auto create = g_create_a.load(std::memory_order_acquire);
    HANDLE file = create(path, access, share, security, disposition, flags, templ);
    const DWORD error = GetLastError();
    file = RedirectOpenedScript(file, access, share, security, disposition, flags, templ);
    SetLastError(error);
    return file;
}
}
bool InitializeInventoryScriptHook(std::shared_ptr<const InventoryScriptCandidate> candidate,
                                   bool write_enabled) noexcept {
    if (!candidate) return false;
    if (g_inventory_started.exchange(true)) return g_inventory_active.load();
    // Never unpublish ownership or remove a trampoline after enable was
    // attempted: an in-flight hook may still use it, even after rollback.
    g_inventory_owner = std::move(candidate);
    g_inventory_candidate.store(g_inventory_owner.get(), std::memory_order_release);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return false;
    void* wide = reinterpret_cast<void*>(GetProcAddress(kernel, "CreateFileW"));
    void* ansi = reinterpret_cast<void*>(GetProcAddress(kernel, "CreateFileA"));
    if (!wide || !ansi || wide == ansi) return false;
    auto status = MH_Initialize(); g_inventory_mh = status;
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    void* trampoline{};
    status = MH_CreateHook(wide, reinterpret_cast<void*>(&HookInventoryCreateW), &trampoline);
    g_inventory_mh = status;
    if (status != MH_OK) return false;
    g_create_w.store(reinterpret_cast<CreateWide>(trampoline), std::memory_order_release);
    status = MH_CreateHook(ansi, reinterpret_cast<void*>(&HookInventoryCreateA), &trampoline);
    g_inventory_mh = status;
    if (status != MH_OK) { (void)MH_RemoveHook(wide); return false; }
    g_create_a.store(reinterpret_cast<CreateAnsi>(trampoline), std::memory_order_release);
    status = MH_QueueEnableHook(wide);
    if (status == MH_OK) status = MH_QueueEnableHook(ansi);
    if (status == MH_OK) status = MH_ApplyQueued();
    g_inventory_mh = status;
    if (status != MH_OK) {
        (void)MH_QueueDisableHook(wide); (void)MH_QueueDisableHook(ansi); (void)MH_ApplyQueued();
        return false;
    }
    g_inventory_write.store(write_enabled, std::memory_order_relaxed);
    g_inventory_active.store(true, std::memory_order_release);
    return true;
}
InventoryScriptHookStats GetInventoryScriptHookStats() noexcept {
    return {g_inventory_matched.load(), g_inventory_redirected.load(), g_inventory_observed.load(),
            g_inventory_failures.load(), g_inventory_active.load(), g_inventory_mh.load()};
}
}
