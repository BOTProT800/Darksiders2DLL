#include "native_texture.h"
#include "general_dds_candidate.h"
#include "resolver_probe_filter.h"
#include "signature_scan.h"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <intrin.h>
#include <limits>
#include <string>

namespace ds2::modding {
namespace {
bool HashMemory(const void* data, std::size_t size, Sha256Digest& hash) noexcept {
    if (!data || !size) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(data);
    if (size > (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
    PortableSha256Context context;
    std::array<std::byte, 16384> chunk;
    for (std::size_t offset=0; offset<size;) {
        const auto n=(std::min)(size-offset,chunk.size()); SIZE_T read{};
        if (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(base+offset),
            chunk.data(),n,&read) || read!=n || !PortableSha256Update(context,{chunk.data(),n})) return false;
        offset+=n;
    }
    return PortableSha256Finalize(context,hash);
}
struct PreserveError { DWORD value=GetLastError(); ~PreserveError() { SetLastError(value); } };
constexpr unsigned kArmed=1, kRejected=2, kObserved=4, kApplied=8, kFallback=16;
std::shared_ptr<const GeneralDdsCandidateSnapshot> g_owner;
std::unique_ptr<NativeTextureRuntime> g_runtime_owner;
std::atomic<NativeTextureRuntime*> g_runtime{};
std::atomic<NativeTextureUploadFn> g_original{};
std::uintptr_t g_image_base{};
std::atomic<bool> g_initialized{};

bool HookNativeTexture(void* object, void* device, std::uint16_t width, std::uint16_t height,
    std::uint32_t format, std::uint16_t mips, const void* data, std::uint32_t flags) {
    const auto original=g_original.load(std::memory_order_acquire);
    if (!original) return false;
    auto* runtime=g_runtime.load(std::memory_order_acquire);
    // The resource-task callsite was disassembled along with the upload ABI.
    // Exclude other engine users of this helper, including generated textures.
    if (!runtime || reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != g_image_base+0x1088457)
        return original(object,device,width,height,format,mips,data,flags);
    return runtime->Upload(original,object,device,width,height,format,mips,data,flags);
}
}
struct NativeTextureRuntime::Slot {
    const GeneralDdsCandidate* candidate{};
    std::atomic<std::uintptr_t> payload{};
    std::atomic<unsigned> events{};
};
NativeTextureRuntime::NativeTextureRuntime(std::span<const GeneralDdsCandidate> candidates) {
    count_=static_cast<std::size_t>(std::count_if(candidates.begin(),candidates.end(),
        [](const auto& c) { return c.requires_native_upload; }));
    slots_=std::make_unique<Slot[]>(count_);
    std::size_t i=0;
    for (const auto& c:candidates) if (c.requires_native_upload) slots_[i++].candidate=&c;
}
NativeTextureRuntime::~NativeTextureRuntime() = default;

void NativeTextureRuntime::Forget(const void* destination, std::size_t size) noexcept {
    const auto begin=reinterpret_cast<std::uintptr_t>(destination);
    if (!begin || !size) return;
    const auto max=(std::numeric_limits<std::uintptr_t>::max)();
    const auto end=size>max-begin ? max : begin+size;
    for (std::size_t i=0;i<count_;++i) {
        auto p=slots_[i].payload.load(std::memory_order_acquire);
        const auto length=slots_[i].candidate->original_dds.payload_size;
        if (p && p<end && (p>=begin || length>begin-p))
            slots_[i].payload.compare_exchange_strong(p,0,std::memory_order_acq_rel);
    }
}
void NativeTextureRuntime::Observe(const ResourceIdentitySample& sample, const void* destination,
    std::int32_t requested, std::int32_t returned) noexcept {
    PreserveError error;
    if (!IsResourceIdentitySampleUsable(sample,0x9FA980,0x9FF0D2) ||
        !destination || requested<=0 || returned!=requested) return;
    for (std::size_t i=0;i<count_;++i) {
        auto& slot=slots_[i]; const auto& c=*slot.candidate;
        if (c.identity.package_base!=sample.scope.package_base ||
            static_cast<std::int64_t>(c.identity.member_table_offset)!=sample.scope.member_table_offset ||
            c.identity.member_ordinal!=sample.read.member_ordinal) continue;
        const bool full=static_cast<std::uint64_t>(requested)==c.original_dds.expected_file_size;
        if (!full && static_cast<std::uint64_t>(requested)!=c.original_dds.payload_size) return;
        Sha256Digest hash{};
        if (!HashMemory(destination,static_cast<std::size_t>(requested),hash) ||
            hash!=(full ? c.original_full_sha256 : c.original_payload_sha256)) {
            slot.events.fetch_or(kRejected); return;
        }
        auto p=reinterpret_cast<std::uintptr_t>(destination);
        if (full) p+=static_cast<std::uintptr_t>(c.original_dds.header_size);
        slot.payload.store(p,std::memory_order_release);
        slot.events.fetch_or(kArmed); return;
    }
}
bool NativeTextureRuntime::Upload(NativeTextureUploadFn original, void* object, void* device,
    std::uint16_t width, std::uint16_t height, std::uint32_t format,
    std::uint16_t mips, const void* data, std::uint32_t flags) {
    first_upload_.store(true);
    const DWORD entry_error=GetLastError();
    const auto pass=[&] { SetLastError(entry_error);
        return original(object,device,width,height,format,mips,data,flags); };
    if (!original) return false;
    Slot* found=nullptr; bool ambiguous=false;
    const auto address=reinterpret_cast<std::uintptr_t>(data);
    if (!address) return pass();
    for (std::size_t i=0;i<count_;++i) {
        auto expected=address;
        if (slots_[i].payload.compare_exchange_strong(expected,0,std::memory_order_acq_rel)) {
            if (found) ambiguous=true;
            found=&slots_[i];
        }
    }
    if (!found || ambiguous) return pass();
    const auto& c=*found->candidate;
    const auto& d=c.original_dds;
    Sha256Digest hash{};
    if (width!=d.width || height!=d.height || mips!=d.mip_count || flags!=0 ||
        format!=NativeTextureFormat(d.format) ||
        !HashMemory(data,static_cast<std::size_t>(d.payload_size),hash) || hash!=c.original_payload_sha256) {
        found->events.fetch_or(kRejected); return pass();
    }
    found->events.fetch_or(kObserved);
    if (!write_enabled.load(std::memory_order_acquire)) return pass();
    const auto replacement=c.storage->Bytes().subspan(c.payload_offset,c.payload_size);
    SetLastError(entry_error);
    if (original(object,device,static_cast<std::uint16_t>(c.dds.width),
        static_cast<std::uint16_t>(c.dds.height),NativeTextureFormat(c.dds.format),
        static_cast<std::uint16_t>(c.dds.mip_count),replacement.data(),flags)) {
        found->events.fetch_or(kApplied); return true;
    }
    // The native helper releases its previous GPU objects before each attempt.
    // Its failure path never takes ownership of the supplied CPU pixel pointer.
    found->events.fetch_or(kFallback);
    return pass();
}
void NativeTextureRuntime::Drain(NativeTextureLogFn log) {
    if (!log) return;
    if (first_upload_.exchange(false)) log(L"NATIVE_TEXTURE_UPLOAD_SEEN",L"rva=0xD4E62C caller=0x1088457");
    for (std::size_t i=0;i<count_;++i) {
        const unsigned events=slots_[i].events.exchange(0);
        if (!events) continue;
        const auto& c=*slots_[i].candidate;
        const auto detail=L"path="+c.virtual_path.key+L" original="+
            std::to_wstring(c.original_dds.width)+L"x"+std::to_wstring(c.original_dds.height)+
            L" replacement="+std::to_wstring(c.dds.width)+L"x"+std::to_wstring(c.dds.height)+
            L" mips="+std::to_wstring(c.dds.mip_count)+L" stream_buffer_written=false";
        if (events&kArmed) log(L"NATIVE_TEXTURE_SOURCE_VERIFIED",detail);
        if (events&kRejected) log(L"NATIVE_TEXTURE_CONTRACT_REJECTED",detail);
        if (events&kObserved) log(L"NATIVE_TEXTURE_VERIFIED_UPLOAD",detail);
        if (events&kApplied) log(L"NATIVE_TEXTURE_OVERRIDE_HIT",detail);
        if (events&kFallback) log(L"NATIVE_TEXTURE_FALLBACK",detail);
    }
}
bool InitializeNativeTextureHook(HMODULE module,
    std::shared_ptr<const GeneralDdsCandidateSnapshot> candidates) noexcept {
    if (!module || !candidates) return false;
    if (g_initialized.exchange(true)) return g_runtime.load()!=nullptr;
    try {
        const auto pattern=ParseSignature(kNativeTextureUploadSignature);
        const auto scan=ScanExecutableSectionsUnique(module,pattern.pattern);
        const auto base=reinterpret_cast<std::uintptr_t>(module);
        if (!scan || reinterpret_cast<std::uintptr_t>(scan.address)!=base+kNativeTextureUploadRva) return false;
        // Prove the resource-task argument setup and exact return address too.
        const auto caller=ParseSignature("89 44 24 20 44 0F B7 4B 0A 44 0F B7 43 08 48 8B CF E8 D5 61 CC FF");
        const auto site=ScanExecutableSectionsUnique(module,caller.pattern);
        if (!site || reinterpret_cast<std::uintptr_t>(site.address)!=base+0x1088441) return false;
        g_owner=std::move(candidates);
        g_runtime_owner=std::make_unique<NativeTextureRuntime>(g_owner->Entries());
        const auto status=MH_Initialize();
        if (status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED) return false;
        void* trampoline{};
        auto* target=const_cast<std::byte*>(scan.address);
        if (MH_CreateHook(target,reinterpret_cast<void*>(&HookNativeTexture),&trampoline)!=MH_OK) return false;
        g_original.store(reinterpret_cast<NativeTextureUploadFn>(trampoline),std::memory_order_release);
        g_image_base=base;
        if (MH_EnableHook(target)!=MH_OK) return false;
        g_runtime.store(g_runtime_owner.get(),std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
void EnableNativeTextureWrites(bool enabled) noexcept {
    if (auto* runtime=g_runtime.load(std::memory_order_acquire)) runtime->write_enabled.store(enabled);
}
void ForgetNativeTextureRead(const void* data, std::int32_t size) noexcept {
    if (size>0) if (auto* runtime=g_runtime.load(std::memory_order_acquire)) runtime->Forget(data,static_cast<std::size_t>(size));
}
void ObserveNativeTextureRead(const ResourceIdentitySample& sample,const void* data,std::int32_t size,std::int32_t returned) noexcept {
    if (auto* runtime=g_runtime.load(std::memory_order_acquire)) runtime->Observe(sample,data,size,returned);
}
void DrainNativeTextureEvents(NativeTextureLogFn log) {
    if (auto* runtime=g_runtime.load(std::memory_order_acquire)) runtime->Drain(log);
}
}
