#include "inventory_script.h"
#include <algorithm>
#include <cstring>

namespace ds2::modding {
namespace {
std::uint32_t Read32(const std::byte* p) noexcept {
    std::uint32_t value{};
    std::memcpy(&value, p, sizeof(value));
    return value; // The supported executable and host are Windows x64 LE.
}
bool PinFile(const std::filesystem::path& path, const ByteStorage& expected,
             HANDLE& handle, FILE_ID_INFO& id) {
    // The supported game's file readers use GENERIC_READ/FILE_SHARE_READ.
    // Real data-read pins are required: attribute-only handles do NOT prevent
    // another handle from writing, regardless of their share flags.
    handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) ||
        !GetFileSizeEx(handle, &size) || size.QuadPart != kInventoryScriptSize) return false;
    std::vector<std::byte> bytes(kInventoryScriptSize);
    DWORD read{};
    if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size()) return false;
    const auto hash = ComputeSha256(bytes);
    return hash && hash.digest == expected.Sha256();
}
}
InventoryScriptValidation ValidateInventoryScriptEdit(
    const std::span<const std::byte> original, const std::span<const std::byte> replacement) {
    InventoryScriptValidation result;
    if (original.size() != kInventoryScriptSize || replacement.size() != original.size()) {
        result.error = L"scripts.obsp size mismatch"; return result;
    }
    // Every byte outside these seven int32 operands must remain identical.
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < kInventorySlotFields.size(); ++i) {
        const auto& field = kInventorySlotFields[i];
        if (std::memcmp(original.data() + field.offset - 10, "NumSlots\0\x23", 10) != 0 ||
            Read32(original.data() + field.offset) != field.original ||
            original[field.offset + 4] != std::byte{0x29} ||
            original[field.offset + 5] != std::byte{0x32}) {
            result.error = L"original NumSlots instruction mismatch"; return result;
        }
        if (!std::equal(original.begin() + cursor, original.begin() + field.offset,
                        replacement.begin() + cursor)) {
            result.error = L"scripts.obsp changes bytes outside NumSlots"; return result;
        }
        const auto value = Read32(replacement.data() + field.offset);
        if (value < field.original || value > kMaximumInventorySlots) {
            result.error = L"NumSlots outside permitted range: " + std::wstring(field.name); return result;
        }
        result.slots[i] = value;
        result.changed |= value != field.original;
        cursor = field.offset + 4;
    }
    if (!std::equal(original.begin() + cursor, original.end(), replacement.begin() + cursor)) {
        result.error = L"scripts.obsp changes bytes outside NumSlots"; return result;
    }
    result.valid = true;
    return result;
}
InventoryScriptCandidate::~InventoryScriptCandidate() {
    if (replacement_pin_ != INVALID_HANDLE_VALUE) CloseHandle(replacement_pin_);
    if (original_pin_ != INVALID_HANDLE_VALUE) CloseHandle(original_pin_);
}
struct InventoryScriptCandidateAccess {
    static InventoryScriptBuildResult Build(const std::filesystem::path& game,
                                             const ModIndexSnapshot& mods) {
        InventoryScriptBuildResult result;
        const auto* asset = mods.Find(kInventoryScriptPath);
        if (!asset) return result;
        const auto path = game / L"media" / L"scripts.obsp";
        const auto original = LoadByteStorageUnderRoot(game, path, kInventoryScriptSize);
        if (!original || !Sha256EqualsHex(original.storage->Sha256(), kInventoryScriptSha256)) {
            result.error = L"unsupported original scripts.obsp SHA-256"; return result;
        }
        const auto validation = ValidateInventoryScriptEdit(original.storage->Bytes(), asset->storage->Bytes());
        if (!validation.valid || !validation.changed) {
            result.error = validation.valid ? L"scripts.obsp has no NumSlots changes" : validation.error;
            return result;
        }
        auto candidate = std::shared_ptr<InventoryScriptCandidate>(new InventoryScriptCandidate);
        // The index and LoadByteStorageUnderRoot already checked containment.
        // Rehash the pinned file objects to close the index/open race.
        if (!PinFile(path, *original.storage, candidate->original_pin_, candidate->original_id_) ||
            !PinFile(asset->source_path, *asset->storage, candidate->replacement_pin_, candidate->replacement_id_)) {
            result.error = L"cannot pin scripts, or file changed since validation"; return result;
        }
        candidate->replacement_path_ = asset->source_path;
        candidate->mod_id_ = asset->mod_id;
        candidate->slots_ = validation.slots;
        result.candidate = std::move(candidate);
        return result;
    }
};
InventoryScriptBuildResult BuildInventoryScriptCandidate(
    const std::filesystem::path& game_directory, const ModIndexSnapshot& mods) {
    return InventoryScriptCandidateAccess::Build(game_directory, mods);
}
}
