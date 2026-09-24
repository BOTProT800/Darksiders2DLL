#pragma once
#include "byte_storage.h"
#include "mod_index.h"
#include <Windows.h>
#include <array>

namespace ds2::modding {
inline constexpr std::wstring_view kInventoryScriptPath = L"media/scripts.obsp";
inline constexpr std::wstring_view kInventoryScriptSha256 =
    L"B46DD3DA7F17A0ED016E30AF523BFBA86D358C9920DFFA866C69D4A0F4F67C7C";
inline constexpr std::size_t kInventoryScriptSize = 18'334'463;
inline constexpr std::uint32_t kMaximumInventorySlots = 255;
struct InventorySlotField final {
    std::wstring_view name;
    std::size_t offset;
    std::uint32_t original;
};
inline constexpr std::array<InventorySlotField, 7> kInventorySlotFields{{
    {L"PrimaryWeapon", 0x80B242, 21}, {L"SecondaryWeapon", 0x80B2C2, 21},
    {L"Shoulder", 0x80B33B, 21}, {L"BodyArmor", 0x80B3B5, 22},
    {L"Gauntlet", 0x80B42E, 22}, {L"Boot", 0x80B4A3, 22},
    {L"Talisman", 0x80B51C, 21},
}};
struct InventoryScriptValidation final {
    bool valid{};
    bool changed{};
    std::array<std::uint32_t, 7> slots{};
    std::wstring error;
};
// Structural/diff check, independent of the build gate. Production must ALSO
// authenticate the complete original using kInventoryScriptSha256.
[[nodiscard]] InventoryScriptValidation ValidateInventoryScriptEdit(
    std::span<const std::byte> original, std::span<const std::byte> replacement);

class InventoryScriptCandidate final {
public:
    ~InventoryScriptCandidate();
    InventoryScriptCandidate(const InventoryScriptCandidate&) = delete;
    InventoryScriptCandidate& operator=(const InventoryScriptCandidate&) = delete;
    [[nodiscard]] const std::filesystem::path& ReplacementPath() const noexcept { return replacement_path_; }
    [[nodiscard]] const FILE_ID_INFO& OriginalId() const noexcept { return original_id_; }
    [[nodiscard]] const FILE_ID_INFO& ReplacementId() const noexcept { return replacement_id_; }
    [[nodiscard]] const auto& Slots() const noexcept { return slots_; }
    [[nodiscard]] const std::wstring& ModId() const noexcept { return mod_id_; }
private:
    InventoryScriptCandidate() = default;
    friend struct InventoryScriptCandidateAccess;
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
    friend struct InventoryScriptTestAccess;
#endif
    HANDLE original_pin_{INVALID_HANDLE_VALUE}, replacement_pin_{INVALID_HANDLE_VALUE};
    FILE_ID_INFO original_id_{}, replacement_id_{};
    std::filesystem::path replacement_path_;
    std::wstring mod_id_;
    std::array<std::uint32_t, 7> slots_{};
};
struct InventoryScriptBuildResult final {
    std::shared_ptr<const InventoryScriptCandidate> candidate;
    std::wstring error;
};
// Uses the index's lexical winner. Invalid winning scripts fail closed; there
// is no fallback to a lower-priority mod. Both files stay read-locked for the
// lifetime of the candidate and are rehashed through those exact handles.
[[nodiscard]] InventoryScriptBuildResult BuildInventoryScriptCandidate(
    const std::filesystem::path& game_directory, const ModIndexSnapshot& mods);
}
