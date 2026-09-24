#include "inventory_script_tests.h"
#include "loader_config.h"
#include "../inventory_script_hook.cpp" // Exercise the actual hook; not linked twice.
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace ds2::modding {
// Synthetic file identities can only be injected in the test executable.
struct InventoryScriptTestAccess {
    static std::shared_ptr<const InventoryScriptCandidate> Make(
        const std::filesystem::path& original, const std::filesystem::path& replacement) {
        auto c = std::shared_ptr<InventoryScriptCandidate>(new InventoryScriptCandidate);
        const auto pin = [](const auto& path, HANDLE& handle, FILE_ID_INFO& id) {
            handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)))
                throw std::runtime_error("inventory fixture pin failed");
        };
        pin(original, c->original_pin_, c->original_id_);
        pin(replacement, c->replacement_pin_, c->replacement_id_);
        c->replacement_path_ = replacement;
        return c;
    }
};
}
namespace {
using namespace ds2::modding;
void Verify(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void WriteFileBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Verify(bool(file), "inventory fixture write failed");
}
void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, 4);
}
struct TestHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~TestHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
unsigned ReadFirst(HANDLE file) {
    Verify(file != INVALID_HANDLE_VALUE, "inventory open failed");
    unsigned char byte{}; DWORD count{};
    Verify(ReadFile(file, &byte, 1, &count, nullptr) && count == 1, "inventory read failed");
    return byte;
}
// Return an unrelated file in place of the validated mod, simulating a parent
// directory replacement after initialization. Production must reject its ID.
std::filesystem::path injected_path;
HANDLE WINAPI WrongReplacement(LPCWSTR, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    return CreateFileW(injected_path.c_str(), access, share, security, disposition, flags, templ);
}
HANDLE WINAPI FailedReplacement(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
}
HANDLE WINAPI FailOnlyMod(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                         DWORD disposition, DWORD flags, HANDLE templ) {
    if (injected_path == path) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    HANDLE file = CreateFileW(path, access, share, security, disposition, flags, templ);
    SetLastError(2468); return file;
}
void ResetInventoryHooks() {
    g_inventory_active = false;
    g_inventory_candidate = nullptr;
    g_inventory_owner.reset();
    g_create_w = &CreateFileW; g_create_a = &CreateFileA;
    g_inventory_write = true;
    g_inventory_matched = 0; g_inventory_redirected = 0;
    g_inventory_observed = 0; g_inventory_failures = 0;
}
void TestOpenRouting(const std::filesystem::path& root) {
    const auto source = root / L"game/media/scripts.obsp";
    const auto mod = root / L"mods/010_slots/media/scripts.obsp";
    const auto other = root / L"unrelated/scripts.obsp";
    const std::array<std::byte, 4> a{std::byte{21}, std::byte{22}, std::byte{23}, std::byte{24}};
    const std::array<std::byte, 4> b{std::byte{42}, std::byte{43}, std::byte{44}, std::byte{45}};
    WriteFileBytes(source, a); WriteFileBytes(mod, b); WriteFileBytes(other, a);
    ResetInventoryHooks();
    auto candidate = InventoryScriptTestAccess::Make(source, mod);
    g_inventory_owner = candidate; g_inventory_candidate = candidate.get(); g_inventory_active = true;
    const auto open = [&](const auto& path, DWORD share = FILE_SHARE_READ, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
        return HookInventoryCreateW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, flags, nullptr);
    };
    {
        TestHandle file{open(source)};
        Verify(ReadFirst(file.value) == 42, "script open was not redirected");
        LARGE_INTEGER length{};
        Verify(GetFileSizeEx(file.value, &length) && length.QuadPart == 4, "redirect size changed");
        LARGE_INTEGER seek{}; seek.QuadPart = 2;
        Verify(SetFilePointerEx(file.value, seek, nullptr, FILE_BEGIN) && ReadFirst(file.value) == 44,
               "redirected seek failed");
        TestHandle mapping{CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr)};
        Verify(mapping.value != nullptr, "script mapping failed");
        const auto view = static_cast<const unsigned char*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0));
        Verify(view != nullptr && view[0] == 42 && view[3] == 45, "script map not replacement");
        UnmapViewOfFile(view);
    }
    { TestHandle file{open(source, 0)};
      Verify(file.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
             "exclusive open unexpectedly bypassed read pins"); }
    { TestHandle file{open(other)}; Verify(ReadFirst(file.value) == 21, "unrelated script redirected"); }
    {
        const auto ansi = source.string();
        TestHandle file{HookInventoryCreateA(ansi.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Verify(ReadFirst(file.value) == 42, "ANSI script open not redirected");
    }
    {
        TestHandle file{open(source, FILE_SHARE_READ, FILE_FLAG_OVERLAPPED)};
        Verify(file.value != INVALID_HANDLE_VALUE, "overlapped open failed");
        TestHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED overlapped{}; overlapped.hEvent = event.value; overlapped.Offset = 1;
        std::array<unsigned char, 2> bytes{}; DWORD count{};
        const BOOL read = ReadFile(file.value, bytes.data(), 2, &count, &overlapped);
        Verify(read || GetLastError() == ERROR_IO_PENDING, "overlapped read rejected");
        Verify(GetOverlappedResult(file.value, &overlapped, &count, TRUE) && count == 2 &&
               bytes[0] == 43 && bytes[1] == 44, "overlapped script bytes incorrect");
    }
    g_inventory_write = false;
    { TestHandle file{open(source)}; Verify(ReadFirst(file.value) == 21, "observe changed script"); }
    Verify(g_inventory_observed == 1, "observe was not counted");
    g_inventory_write = true;
    { TestHandle file{open(source, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT)};
      Verify(ReadFirst(file.value) == 21, "reparse-point open redirected"); }
    Verify(!EligibleOpen(GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, nullptr) &&
           !EligibleOpen(GENERIC_READ, OPEN_ALWAYS, 0, nullptr) &&
           !EligibleOpen(GENERIC_READ, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr),
           "unsafe open accepted");
    {
        TestHandle original{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, 0, nullptr)};
        g_create_w = &FailedReplacement;
        HANDLE result = RedirectOpenedScript(original.value, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Verify(result == original.value && ReadFirst(result) == 21, "failed mod open lost original handle");
        injected_path = other; g_create_w = &WrongReplacement;
        result = RedirectOpenedScript(original.value, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Verify(result == original.value, "replacement file ID mismatch accepted");
        g_create_w = &CreateFileW;
    }
    Verify(g_inventory_failures == 2, "fallback failures not counted");
    {
        injected_path = mod; g_create_w = &FailOnlyMod;
        TestHandle file{open(source)};
        Verify(GetLastError() == 2468 && ReadFirst(file.value) == 21,
               "replacement failure did not preserve original open/error");
        g_create_w = &CreateFileW;
    }
    {
        TestHandle file{open(source, FILE_SHARE_READ, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING)};
        Verify(file.value != INVALID_HANDLE_VALUE, "native unbuffered open failed");
        TestHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED overlapped{}; overlapped.hEvent = event.value;
        void* aligned = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Verify(aligned != nullptr, "unbuffered allocation failed");
        DWORD count{};
        const BOOL read = ReadFile(file.value, aligned, 4096, &count, &overlapped);
        const bool started = read || GetLastError() == ERROR_IO_PENDING;
        const bool completed = started && GetOverlappedResult(file.value, &overlapped, &count, TRUE);
        const bool correct = completed && count == 4 && static_cast<unsigned char*>(aligned)[0] == 42;
        VirtualFree(aligned, 0, MEM_RELEASE);
        Verify(correct, "native unbuffered+overlapped read failed");
    }
    {
        const auto missing = root / L"missing.obsp";
        TestHandle file{open(missing)};
        Verify(file.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND,
               "failed source open error changed");
        for (const auto& path : {source, mod}) {
            TestHandle writer{CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, 0, nullptr)};
            Verify(writer.value == INVALID_HANDLE_VALUE, "validated script allowed writer");
        }
    }
    // Install the real MinHook detours in this disposable test process too.
    // This exercises activation and actual A/W exports, not only direct calls.
    g_inventory_active = false;
    Verify(InitializeInventoryScriptHook(candidate, true), "inventory MinHook activation failed");
    {
        TestHandle file{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, 0, nullptr)};
        Verify(ReadFirst(file.value) == 42, "installed wide detour failed");
        const auto path = source.string();
        TestHandle ansi{CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, 0, nullptr)};
        Verify(ReadFirst(ansi.value) == 42, "installed ANSI detour failed");
    }
    g_inventory_active = false;
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    for (const auto name : {"CreateFileW", "CreateFileA"}) {
        void* target = reinterpret_cast<void*>(GetProcAddress(kernel, name));
        Verify(MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK,
               "inventory test detour cleanup failed");
    }
    ResetInventoryHooks(); candidate.reset();
    Verify(LoadByteStorage(source).storage->Bytes()[0] == std::byte{21}, "original script modified on disk");
}
}
void TestInventoryScripts(const std::filesystem::path& root) {
    using namespace ds2::modding;
    Verify(ParseLoaderConfig("[loader]\n").config.scripts == ScriptMode::off, "scripts enabled by default");
    for (const auto mode : {"off", "observe", "inventory"})
        Verify(ParseLoaderConfig(std::string("[loader]\nscripts=") + mode).valid, "valid script mode rejected");
    Verify(!ParseLoaderConfig("[loader]\nscripts=any").valid &&
           !ParseLoaderConfig("[loader]\nscripts=off\nscripts=inventory").valid, "invalid script mode accepted");
    std::vector<std::byte> original(kInventoryScriptSize, std::byte{0x55});
    for (const auto& field : kInventorySlotFields) {
        std::memcpy(original.data() + field.offset - 10, "NumSlots\0\x23", 10);
        Put32(original, field.offset, field.original);
        original[field.offset + 4] = std::byte{0x29}; original[field.offset + 5] = std::byte{0x32};
    }
    auto replacement = original;
    auto validation = ValidateInventoryScriptEdit(original, replacement);
    Verify(validation.valid && !validation.changed, "identical script validation failed");
    for (const auto& field : kInventorySlotFields) Put32(replacement, field.offset, 42);
    Verify(ValidateInventoryScriptEdit(original, replacement).valid, "valid slot edits rejected");
    for (const auto& field : kInventorySlotFields) {
        for (const auto invalid : {0u, field.original - 1, 256u, 0xFFFFFFFFu}) {
            Put32(replacement, field.offset, invalid);
            Verify(!ValidateInventoryScriptEdit(original, replacement).valid, "invalid slot operand accepted");
        }
        Put32(replacement, field.offset, 255);
    }
    Verify(ValidateInventoryScriptEdit(original, replacement).valid, "max slots rejected");
    for (const auto offset : {std::size_t{0}, kInventorySlotFields[0].offset - 1,
         kInventorySlotFields[0].offset + 4, kInventoryScriptSize - 1}) {
        replacement[offset] ^= std::byte{1};
        Verify(!ValidateInventoryScriptEdit(original, replacement).valid, "non-slot edit accepted");
        replacement[offset] ^= std::byte{1};
    }
    Verify(!ValidateInventoryScriptEdit(original, std::span(replacement).first(replacement.size() - 1)).valid,
           "truncated script accepted");
    auto corrupt_original = original;
    corrupt_original[kInventorySlotFields[2].offset - 2] ^= std::byte{1};
    Verify(!ValidateInventoryScriptEdit(corrupt_original, replacement).valid, "bad original instruction accepted");
    const auto fixture = root / L"inventory-validation";
    const auto mods = fixture / L"mods";
    WriteFileBytes(fixture / L"media/scripts.obsp", original);
    WriteFileBytes(mods / L"010_slots/media/scripts.obsp", replacement);
    WriteFileBytes(mods / L"020_slots/media/scripts.obsp", original);
    WriteFileBytes(mods / L"010_slots/media/other.obsp", replacement);
    ModIndexOptions options; options.dds_only = true;
    auto index = BuildModIndex(mods, options);
    Verify(index && index.snapshot->Assets().empty(), "script indexing enabled by default");
    options.include_inventory_scripts = true; index = BuildModIndex(mods, options);
    Verify(index && index.snapshot->Assets().size() == 2, "script index accepted wrong path");
    Verify(index.snapshot->Find(kInventoryScriptPath)->mod_id == L"010_slots", "script priority incorrect");
    const auto rejected = BuildInventoryScriptCandidate(fixture, *index.snapshot);
    Verify(!rejected.candidate && !rejected.error.empty(), "untrusted original passed production hash gate");
    TestOpenRouting(root / L"inventory-routing");
    std::cout << "inventory_scripts: PASS (slot-only diff, hash gate, priority, real A/W hooks, async, unbuffered, mapping, observe, fallback, pins)\n";
}
int CheckInventoryScriptMod(const std::filesystem::path& game, const std::filesystem::path& mods) {
    using namespace ds2::modding;
    ModIndexOptions options; options.dds_only = true; options.include_inventory_scripts = true;
    const auto index = BuildModIndex(mods, options);
    if (!index) { std::cerr << "inventory index failed\n"; return 1; }
    const auto built = BuildInventoryScriptCandidate(game, *index.snapshot);
    if (!built.candidate) {
        std::wcerr << L"inventory check failed: " << (built.error.empty() ? L"no script mod found" : built.error) << L'\n';
        return 1;
    }
    std::wcout << L"inventory-check: PASS mod=" << built.candidate->ModId() << L'\n';
    for (std::size_t i = 0; i < kInventorySlotFields.size(); ++i)
        std::wcout << kInventorySlotFields[i].name << L'=' << built.candidate->Slots()[i] << L'\n';
    return 0;
}
