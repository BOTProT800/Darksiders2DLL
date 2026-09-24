#pragma once
#include "dds_validation.h"
#include "resource_identity.h"
#include <Windows.h>
#include <atomic>
#include <memory>
#include <span>
#include <string_view>

namespace ds2::modding {
inline constexpr std::uint32_t kUnsupportedNativeTextureFormat = 0xffffffffu;
// Engine enum recovered from RVA 0x751260 in build 5580738.
constexpr std::uint32_t NativeTextureFormat(DdsFormat format) noexcept {
    switch (format) {
    case DdsFormat::bgra8: return 6;
    case DdsFormat::bc1: return 15;
    case DdsFormat::bc2: return 16;
    case DdsFormat::bc3: return 17;
    default: return kUnsupportedNativeTextureFormat;
    }
}
inline constexpr std::uintptr_t kNativeTextureUploadRva = 0xD4E62C;
inline constexpr char kNativeTextureUploadSignature[] =
    "48 8B C4 66 44 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 "
    "41 54 41 55 41 56 41 57 48 8D 68 B9 48 81 EC 88 00 00 00";

struct GeneralDdsCandidate;
class GeneralDdsCandidateSnapshot;
using NativeTextureUploadFn = bool(*)(void*, void*, std::uint16_t, std::uint16_t,
    std::uint32_t, std::uint16_t, const void*, std::uint32_t);
using NativeTextureLogFn = void(*)(std::wstring_view, std::wstring_view) noexcept;

// Testable production path. Ownership of the candidate span must outlive this
// object. Arming never modifies original data. Upload consumes the registration
// and rechecks identity-bound bytes plus the engine's dimensions/format/mips.
class NativeTextureRuntime final {
public:
    explicit NativeTextureRuntime(std::span<const GeneralDdsCandidate> candidates);
    ~NativeTextureRuntime();
    void Forget(const void* destination, std::size_t size) noexcept;
    void Observe(const ResourceIdentitySample&, const void*, std::int32_t, std::int32_t) noexcept;
    bool Upload(NativeTextureUploadFn, void*, void*, std::uint16_t, std::uint16_t,
        std::uint32_t, std::uint16_t, const void*, std::uint32_t);
    void Drain(NativeTextureLogFn);
    std::atomic<bool> write_enabled{false};
private:
    struct Slot;
    std::unique_ptr<Slot[]> slots_;
    std::size_t count_{};
    std::atomic<bool> first_upload_{};
};

[[nodiscard]] bool InitializeNativeTextureHook(HMODULE,
    std::shared_ptr<const GeneralDdsCandidateSnapshot>) noexcept;
void EnableNativeTextureWrites(bool enabled) noexcept;
void ForgetNativeTextureRead(const void*, std::int32_t) noexcept;
void ObserveNativeTextureRead(const ResourceIdentitySample&, const void*, std::int32_t, std::int32_t) noexcept;
void DrainNativeTextureEvents(NativeTextureLogFn);
}
