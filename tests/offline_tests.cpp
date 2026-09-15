#include "byte_storage.h"
#include "dds_validation.h"
#include "game_build.h"
#include "general_dds_candidate.h"
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
#include "general_resolver_runtime_tests.h"
#endif
#include "logging.h"
#include "loader_config.h"
#include "memory_range.h"
#include "mod_index.h"
#include "package_dds_contract.h"
#include "package_identity_catalog.h"
#include "resource_identity.h"
#include "resolver_probe_filter.h"
#include "sha256.h"
#include "signature_scan.h"
#include "virtual_path.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ds2::modding;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void PutU32(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFF);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<std::byte>((value >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<std::byte>((value >> 24) & 0xFF);
}

void AppendU8(std::vector<std::byte>& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}

void AppendU16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    AppendU8(bytes, static_cast<std::uint8_t>(value));
    AppendU8(bytes, static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        AppendU8(bytes, static_cast<std::uint8_t>(value >> shift));
    }
}

void AppendU64(std::vector<std::byte>& bytes, const std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        AppendU8(bytes, static_cast<std::uint8_t>(value >> shift));
    }
}

void AppendString(std::vector<std::byte>& bytes, const std::string_view value) {
    for (const char character : value) {
        AppendU8(bytes, static_cast<std::uint8_t>(character));
    }
}

struct SyntheticObpk final {
    std::vector<std::byte> bytes;
    std::uint32_t payload_offset{};
    std::uint32_t names_end{};
    std::size_t names_flag_offset{};
    std::size_t first_name_offset{};
};

SyntheticObpk MakeSyntheticNamedObpk(
    const bool member_blocks = false,
    const std::uint32_t type_id = 6,
    const std::array<std::uint32_t, 2> sizes = {144, 144}) {
    constexpr std::uint32_t table_offset = 0x40;
    constexpr std::array<std::string_view, 2> names{"first", "second"};

    SyntheticObpk fixture;
    fixture.bytes.resize(table_offset);
    AppendU32(fixture.bytes, 1);  // group count
    AppendU32(fixture.bytes, static_cast<std::uint32_t>(names.size()));
    AppendU32(fixture.bytes, 0);  // hash table size
    AppendU32(fixture.bytes, 0);
    for (unsigned int index = 0; index < 4; ++index) {
        AppendU32(fixture.bytes, 0);
    }
    AppendU32(fixture.bytes, type_id);
    AppendU32(fixture.bytes, static_cast<std::uint32_t>(names.size()));
    AppendU32(fixture.bytes, 0);
    AppendU32(fixture.bytes, 0);
    fixture.bytes.resize(fixture.bytes.size() + names.size());
    fixture.names_flag_offset = fixture.bytes.size();
    AppendU8(fixture.bytes, 1);  // names are present
    AppendU32(fixture.bytes, 6);  // longest name
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto name = names[index];
        AppendU32(fixture.bytes, 0);
        AppendU32(fixture.bytes, 0);
        AppendU32(fixture.bytes, static_cast<std::uint32_t>(name.size()));
        if (index == 0) {
            fixture.first_name_offset = fixture.bytes.size();
        }
        AppendString(fixture.bytes, name);
    }
    fixture.names_end = static_cast<std::uint32_t>(fixture.bytes.size());

    // Each META record carries the authoritative uncompressed member size.
    for (const auto size : sizes) {
        AppendU32(fixture.bytes, size);
        AppendU32(fixture.bytes, 0);  // no per-member extra metadata
    }
    fixture.payload_offset = static_cast<std::uint32_t>(
        fixture.bytes.size() + sizes.size() * sizeof(std::uint32_t));
    if (member_blocks) {
        AppendU32(fixture.bytes, fixture.payload_offset);
        AppendU32(fixture.bytes, fixture.payload_offset + 8);
    } else {
        for (const auto size : sizes) {
            AppendU32(fixture.bytes, size);
        }
    }
    const auto metadata_size = static_cast<std::uint32_t>(
        fixture.bytes.size() - fixture.names_end);
    if (member_blocks) {
        for (const auto size : sizes) {
            AppendU32(fixture.bytes, size);
            AppendU32(fixture.bytes, 0x785E9C78);  // inert compressed bytes
        }
    } else {
        AppendU32(fixture.bytes, sizes[0] + sizes[1]);
        AppendU32(fixture.bytes, 0x785E9C78);  // inert bytes after the size prefix
    }

    fixture.bytes[0] = std::byte{0x4F};  // "OBPK\0"
    fixture.bytes[1] = std::byte{0x42};
    fixture.bytes[2] = std::byte{0x50};
    fixture.bytes[3] = std::byte{0x4B};
    fixture.bytes[4] = std::byte{0x00};
    PutU32(fixture.bytes, 5, 9);
    PutU32(fixture.bytes, 9, table_offset);
    PutU32(fixture.bytes, 13, fixture.names_end - table_offset);
    PutU32(fixture.bytes, 17, fixture.names_end);
    PutU32(fixture.bytes, 21, metadata_size);
    PutU32(fixture.bytes, 25, fixture.payload_offset);
    // Byte 29 is padding in the structural parser. Keep it zero even for the
    // per-member-block fixture so this test cannot regress to marker guessing.
    fixture.bytes[29] = std::byte{0};
    return fixture;
}

SyntheticObpk MakeSyntheticEmptyObpk() {
    constexpr std::uint32_t table_offset = 0x40;
    SyntheticObpk fixture;
    fixture.bytes.resize(table_offset);
    AppendU32(fixture.bytes, 0);  // group count
    AppendU32(fixture.bytes, 0);  // file count
    AppendU32(fixture.bytes, 0);  // hash table size
    AppendU32(fixture.bytes, 0);
    for (unsigned int index = 0; index < 4; ++index) {
        AppendU32(fixture.bytes, 0);
    }
    fixture.names_flag_offset = fixture.bytes.size();
    AppendU8(fixture.bytes, 0);  // names are absent
    fixture.names_end = static_cast<std::uint32_t>(fixture.bytes.size());
    fixture.payload_offset = fixture.names_end;
    AppendU32(fixture.bytes, 0);  // inert empty payload prefix

    fixture.bytes[0] = std::byte{0x4F};
    fixture.bytes[1] = std::byte{0x42};
    fixture.bytes[2] = std::byte{0x50};
    fixture.bytes[3] = std::byte{0x4B};
    fixture.bytes[4] = std::byte{0x00};
    PutU32(fixture.bytes, 5, 9);
    PutU32(fixture.bytes, 9, table_offset);
    PutU32(fixture.bytes, 13, fixture.names_end - table_offset);
    PutU32(fixture.bytes, 17, fixture.names_end);
    PutU32(fixture.bytes, 21, 0);
    PutU32(fixture.bytes, 25, fixture.payload_offset);
    fixture.bytes[29] = std::byte{0};
    return fixture;
}

std::vector<std::byte> MakeSyntheticManifest(const std::uint64_t segment_offset) {
    std::vector<std::byte> bytes;
    AppendU32(bytes, 0x0D);  // Deathinitive 64-bit offsets
    for (unsigned int index = 0; index < 10; ++index) {
        AppendU32(bytes, 0);
    }
    AppendU32(bytes, 1);  // path count
    AppendU32(bytes, 0);
    AppendU64(bytes, 0);
    AppendU32(bytes, 2);
    AppendString(bytes, "ui");
    AppendU32(bytes, 1);  // package count
    AppendU8(bytes, 0);
    AppendU16(bytes, 5);
    AppendString(bytes, "media");
    AppendU32(bytes, 1);  // asset/segment count
    AppendU8(bytes, 7);
    AppendString(bytes, "fixture");
    AppendU64(bytes, 0);
    AppendU16(bytes, 0);
    AppendU16(bytes, 0);  // path index
    AppendU8(bytes, 0);   // alias count
    AppendU8(bytes, 0);
    AppendU8(bytes, 0);   // package index
    AppendU8(bytes, 0);
    AppendU8(bytes, 0);
    AppendU64(bytes, segment_offset);
    return bytes;
}

std::vector<std::byte> MakeDxt5Dds(
    const std::uint32_t width,
    const std::uint32_t height) {
    const auto blocks_wide = (static_cast<std::size_t>(width) + 3) / 4;
    const auto blocks_high = (static_cast<std::size_t>(height) + 3) / 4;
    std::vector<std::byte> bytes(128 + blocks_wide * blocks_high * 16);
    PutU32(bytes, 0, 0x20534444);   // "DDS "
    PutU32(bytes, 4, 124);
    PutU32(bytes, 8, 0x00001007);  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    PutU32(bytes, 12, height);
    PutU32(bytes, 16, width);
    PutU32(bytes, 76, 32);
    PutU32(bytes, 80, 4);          // DDPF_FOURCC
    PutU32(bytes, 84, 0x35545844); // "DXT5"
    return bytes;
}

std::vector<std::byte> MakeDxt5Dds4x4() {
    return MakeDxt5Dds(4, 4);
}

std::vector<std::byte> MakeDxt1Dds(
    const std::uint32_t width,
    const std::uint32_t height) {
    const auto blocks_wide = (static_cast<std::size_t>(width) + 3) / 4;
    const auto blocks_high = (static_cast<std::size_t>(height) + 3) / 4;
    std::vector<std::byte> bytes(128 + blocks_wide * blocks_high * 8);
    PutU32(bytes, 0, 0x20534444);   // "DDS "
    PutU32(bytes, 4, 124);
    PutU32(bytes, 8, 0x00001007);  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    PutU32(bytes, 12, height);
    PutU32(bytes, 16, width);
    PutU32(bytes, 76, 32);
    PutU32(bytes, 80, 4);          // DDPF_FOURCC
    PutU32(bytes, 84, 0x31545844); // "DXT1"
    return bytes;
}

std::vector<std::byte> MakeDx10Bc3Dds4x4() {
    std::vector<std::byte> bytes(148 + 16);
    PutU32(bytes, 0, 0x20534444);   // "DDS "
    PutU32(bytes, 4, 124);
    PutU32(bytes, 8, 0x00001007);  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    PutU32(bytes, 12, 4);
    PutU32(bytes, 16, 4);
    PutU32(bytes, 76, 32);
    PutU32(bytes, 80, 4);          // DDPF_FOURCC
    PutU32(bytes, 84, 0x30315844); // "DX10"
    PutU32(bytes, 128, 77);        // DXGI_FORMAT_BC3_UNORM
    PutU32(bytes, 132, 3);         // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    PutU32(bytes, 140, 1);         // one array slice
    return bytes;
}

SyntheticObpk MakeSyntheticDdsStreamObpk(
    const std::array<std::vector<std::byte>, 2>& members) {
    Require(
        members[0].size() <= (std::numeric_limits<std::uint32_t>::max)() &&
            members[1].size() <= (std::numeric_limits<std::uint32_t>::max)(),
        "synthetic DDS member size is not representable");
    const std::array<std::uint32_t, 2> sizes{
        static_cast<std::uint32_t>(members[0].size()),
        static_cast<std::uint32_t>(members[1].size())};
    SyntheticObpk fixture = MakeSyntheticNamedObpk(false, 6, sizes);

    std::vector<std::byte> unpacked;
    unpacked.reserve(members[0].size() + members[1].size());
    unpacked.insert(unpacked.end(), members[0].begin(), members[0].end());
    unpacked.insert(unpacked.end(), members[1].begin(), members[1].end());
    Require(
        unpacked.size() <= (std::numeric_limits<uLong>::max)(),
        "synthetic zlib source is not representable");

    uLongf compressed_size = compressBound(static_cast<uLong>(unpacked.size()));
    std::vector<std::byte> compressed(static_cast<std::size_t>(compressed_size));
    const int status = compress2(
        reinterpret_cast<Bytef*>(compressed.data()),
        &compressed_size,
        reinterpret_cast<const Bytef*>(unpacked.data()),
        static_cast<uLong>(unpacked.size()),
        Z_BEST_COMPRESSION);
    Require(status == Z_OK, "cannot create synthetic zlib stream");
    compressed.resize(static_cast<std::size_t>(compressed_size));

    fixture.bytes.resize(fixture.payload_offset);
    AppendU32(fixture.bytes, static_cast<std::uint32_t>(unpacked.size()));
    fixture.bytes.insert(
        fixture.bytes.end(), compressed.begin(), compressed.end());
    return fixture;
}

void WriteBytes(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary);
    Require(output.good(), "cannot create fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(output.good(), "cannot write fixture");
}

bool HasIssue(
    const ModIndexBuildResult& result,
    const ModIndexIssueCode code) noexcept {
    for (const auto& issue : result.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

bool HasPackageIssue(
    const PackageIdentityCatalogBuildResult& result,
    const PackageIdentityCatalogIssueCode code) noexcept {
    for (const auto& issue : result.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

bool HasPackageDdsContractIssue(
    const PackageDdsContractCatalogBuildResult& result,
    const PackageDdsContractCatalogIssueCode code) noexcept {
    for (const auto& issue : result.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

bool HasGeneralDdsIssue(
    const GeneralDdsCandidateBuildResult& result,
    const GeneralDdsCandidateIssueCode code) noexcept {
    for (const auto& issue : result.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::array<wchar_t, MAX_PATH> base{};
        const DWORD base_length = GetTempPathW(
            static_cast<DWORD>(base.size()), base.data());
        Require(base_length != 0 && base_length < base.size(),
                "GetTempPathW failed or exceeded its buffer");
        for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
            path_ = std::filesystem::path(base.data()) /
                (L"Darksiders2DLL-tests-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        throw std::runtime_error("cannot create temporary directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void TestVirtualPaths() {
    const auto good = NormalizeVirtualPath(L"Media\\UI/Icon.DDS");
    Require(good && good.path->display == L"Media/UI/Icon.DDS", "separator normalization failed");
    Require(good.path->key == L"media/ui/icon.dds", "case normalization failed");
    Require(!NormalizeVirtualPath(L"../outside.dds"), "traversal accepted");
    Require(!NormalizeVirtualPath(L"C:\\outside.dds"), "drive path accepted");
    Require(!NormalizeVirtualPath(L"media/file.dds:stream"), "ADS accepted");
    Require(!NormalizeVirtualPath(L"\\\\server\\share"), "UNC path accepted");
    Require(IsSafeModId(L"first_test_mod"), "known mod id rejected");
}

void TestHashAndBuildFailClosed(const std::filesystem::path& root) {
    const std::array<std::byte, 3> abc{
        static_cast<std::byte>('a'), static_cast<std::byte>('b'), static_cast<std::byte>('c')};
    const auto hash = ComputeSha256(abc);
    Require(static_cast<bool>(hash), "SHA-256 failed");
    Require(Sha256Hex(hash.digest) ==
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
        "SHA-256 vector mismatch");
    Require(!Sha256EqualsHex(hash.digest, kSupportedExecutableSha256),
            "unrelated hash matched supported executable");

    const auto fake_executable = root / L"Darksiders2.exe";
    WriteBytes(fake_executable, abc);
    const auto build = IdentifyGameBuild(fake_executable);
    Require(!build.Supported(), "unknown executable was supported");
    Require(build.error == GameBuildError::unsupported_hash, "unknown hash error mismatch");
    Require(build.sha256_hex == Sha256HexWide(hash.digest), "fingerprint was not reported");
}

void TestDdsValidation() {
    const auto bytes = MakeDxt5Dds4x4();
    const auto valid = ValidateDds(bytes);
    Require(static_cast<bool>(valid), "synthetic DXT5 DDS rejected");
    Require(valid.metadata->format == DdsFormat::bc3, "DDS format mismatch");
    Require(valid.metadata->expected_file_size == 144, "DDS size mismatch");
    const auto truncated = ValidateDds(std::span<const std::byte>(bytes.data(), bytes.size() - 1));
    Require(!truncated && truncated.error == DdsValidationError::size_mismatch,
            "truncated DDS accepted");
}

void TestReadableMemoryRange() {
    Require(!IsReadableMemoryRange(nullptr, 1),
            "null memory range was accepted");
    Require(!IsReadableMemoryRange(reinterpret_cast<const void*>(1), 0),
            "empty memory range was accepted");
    Require(!IsReadableMemoryRange(
                reinterpret_cast<const void*>(
                    (std::numeric_limits<std::uintptr_t>::max)() - 7),
                16),
            "overflowing memory range was accepted");

    std::array<std::byte, 16> stack_bytes{};
    Require(IsReadableMemoryRange(stack_bytes.data(), stack_bytes.size()),
            "readable stack range was rejected");
    Require(IsWritableMemoryRange(stack_bytes.data(), stack_bytes.size()),
            "writable stack range was rejected");

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::size_t page_size = system_info.dwPageSize;
    Require(page_size != 0 && page_size < 4'224,
            "unexpected Windows page size for 4224-byte boundary test");

    void* const allocation = VirtualAlloc(
        nullptr,
        page_size * 2,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    Require(allocation != nullptr, "VirtualAlloc failed for memory range test");
    const auto release_allocation = [](void* const address) noexcept {
        if (address != nullptr) {
            static_cast<void>(VirtualFree(address, 0, MEM_RELEASE));
        }
    };
    const std::unique_ptr<void, decltype(release_allocation)> owned_allocation(
        allocation, release_allocation);

    Require(IsReadableMemoryRange(allocation, 4'224),
            "readable 4224-byte cross-page range was rejected");
    Require(IsWritableMemoryRange(allocation, 4'224),
            "writable 4224-byte cross-page range was rejected");

    auto* const second_page = static_cast<std::byte*>(allocation) + page_size;
    DWORD old_protection = 0;
    Require(VirtualProtect(
                second_page,
                page_size,
                PAGE_NOACCESS,
                &old_protection) != FALSE,
            "VirtualProtect PAGE_NOACCESS failed");
    Require(IsReadableMemoryRange(allocation, page_size),
            "readable prefix was rejected");
    Require(!IsReadableMemoryRange(allocation, 4'224),
            "cross-page range with unreadable tail was accepted");

    DWORD ignored_protection = 0;
    Require(VirtualProtect(
                second_page,
                page_size,
                PAGE_READWRITE | PAGE_GUARD,
                &ignored_protection) != FALSE,
            "VirtualProtect PAGE_GUARD failed");
    Require(!IsReadableMemoryRange(allocation, 4'224),
            "cross-page range with guard tail was accepted");
    Require(!IsWritableMemoryRange(allocation, 4'224),
            "cross-page writable range with guard tail was accepted");
}

void TestSignatureScanner() {
    const auto parsed = ParseSignature("10 20 ?? 40");
    Require(static_cast<bool>(parsed), "valid signature did not parse");
    Require(parsed.pattern.size() == 4 && parsed.pattern[2].wildcard,
            "signature wildcard mismatch");
    Require(!ParseSignature("1G"), "invalid hexadecimal signature accepted");
    Require(!ParseSignature(""), "empty signature accepted");

    const std::array<std::byte, 7> unique_bytes{
        std::byte{0x00}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x40}, std::byte{0x50}, std::byte{0x60}};
    const auto unique = ScanUnique(unique_bytes, parsed.pattern);
    Require(unique.status == SignatureScanStatus::found &&
                unique.address == unique_bytes.data() + 1,
            "unique signature was not found at the expected address");

    const std::array<std::byte, 8> ambiguous_bytes{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
        std::byte{0x10}, std::byte{0x20}, std::byte{0x31}, std::byte{0x40}};
    Require(ScanUnique(ambiguous_bytes, parsed.pattern).status ==
                SignatureScanStatus::ambiguous,
            "ambiguous signature was accepted");

    Require(ScanExecutableSectionsUnique(nullptr, parsed.pattern).status ==
                SignatureScanStatus::invalid_module,
            "null module was accepted by executable scanner");
    Require(ScanExecutableSectionsUnique(
                reinterpret_cast<HMODULE>(std::uintptr_t{1}), parsed.pattern).status ==
                SignatureScanStatus::invalid_module,
            "invalid module address was accepted by executable scanner");
    Require(ScanExecutableSectionsUnique(
                GetModuleHandleW(nullptr), parsed.pattern, ".absent").status ==
                SignatureScanStatus::not_found,
            "missing executable section did not report not_found");
}

void TestResolverProbeFilter() {
    const std::array<std::byte, 3> abc{
        std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};
    const std::array<std::uint8_t, 32> expected_abc_sha256{
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
    };
    Require(PortableSha256(abc) == expected_abc_sha256,
            "portable resolver SHA-256 vector mismatch");

    const std::array<std::size_t, 12> sha_lengths{
        0, 1, 3, 55, 56, 63, 64, 65, 127, 128,
        kTargetBc3PayloadSize, kTargetDdsSize};
    for (const std::size_t length : sha_lengths) {
        std::vector<std::byte> input(length);
        for (std::size_t index = 0; index < input.size(); ++index) {
            input[index] = static_cast<std::byte>(
                (index * 131u + length * 17u) & 0xFFu);
        }
        const Sha256Result system_hash = ComputeSha256(input);
        Require(static_cast<bool>(system_hash),
                "system SHA-256 failed for resolver comparison");
        Require(PortableSha256(input) == system_hash.digest,
                "portable and system SHA-256 results disagree");

        PortableSha256Context incremental;
        Require(PortableSha256Update(incremental, {}),
                "incremental SHA-256 rejected an empty update");
        constexpr std::array<std::size_t, 7> chunk_sizes{
            1, 7, 63, 2, 64, 129, 4'096};
        std::size_t offset = 0;
        std::size_t chunk_index = 0;
        while (offset < input.size()) {
            const std::size_t count = (std::min)(
                chunk_sizes[chunk_index % chunk_sizes.size()],
                input.size() - offset);
            Require(PortableSha256Update(
                        incremental,
                        std::span(input).subspan(offset, count)),
                    "incremental SHA-256 update failed");
            offset += count;
            ++chunk_index;
        }
        std::array<std::uint8_t, 32> incremental_digest{};
        Require(PortableSha256Finalize(
                    incremental, incremental_digest) &&
                    incremental_digest == system_hash.digest,
                "incremental and system SHA-256 results disagree");
        std::array<std::uint8_t, 32> repeated_digest{};
        Require(PortableSha256Finalize(
                    incremental, repeated_digest) &&
                    repeated_digest == incremental_digest,
                "incremental SHA-256 finalization mutated its context");
    }

    PortableSha256Context oversized_context;
    oversized_context.total_size =
        (std::numeric_limits<std::uint64_t>::max)() / 8ull;
    const std::array<std::byte, 1> one_byte{std::byte{0x01}};
    Require(!PortableSha256Update(oversized_context, one_byte),
            "incremental SHA-256 accepted a message beyond its bit-length limit");
    std::array<std::uint8_t, 32> invalid_digest{};
    Require(!PortableSha256Finalize(oversized_context, invalid_digest),
            "invalid incremental SHA-256 context finalized successfully");

    const std::array<std::byte, 5> hello{
        std::byte{0x68}, std::byte{0x65}, std::byte{0x6C},
        std::byte{0x6C}, std::byte{0x6F}};
    Require(Fnv1a64(hello) == 0xA430D84680AABD0Bull,
            "resolver prefilter FNV-1a vector mismatch");

    std::array<std::byte, kTargetDdsSize> dds{};
    dds[0] = std::byte{0x44};
    dds[1] = std::byte{0x44};
    dds[2] = std::byte{0x53};
    dds[3] = std::byte{0x20};
    Require(ClassifyResolverSample(
                static_cast<std::int32_t>(dds.size()),
                static_cast<std::int32_t>(dds.size()),
                dds) == ResolverSampleKind::full_dds,
            "full DDS stream sample was not classified");
    Require(ClassifyResolverSample(
                static_cast<std::int32_t>(dds.size()),
                static_cast<std::int32_t>(dds.size() - 1),
                dds) == ResolverSampleKind::none,
            "short stream read was classified as complete");

    std::array<std::byte, kTargetBc3PayloadSize> unrelated_payload{};
    Require(ClassifyResolverSample(
                static_cast<std::int32_t>(unrelated_payload.size()),
                static_cast<std::int32_t>(unrelated_payload.size()),
                unrelated_payload) == ResolverSampleKind::none,
            "unrelated 4096-byte payload passed the target prefilter");
}

void TestResourceIdentityCorrelation() {
    ResourceIdentityThreadContext context;
    Require(context.Empty(), "new resource identity context is not empty");

    ResourceIdentityScopeInput outer_scope;
    outer_scope.owner = 0x1000;
    outer_scope.argument2 = 0x1100;
    outer_scope.argument3 = 0x1200;
    outer_scope.argument4 = 0x1300;
    outer_scope.argument5 = 0x1400;
    outer_scope.stream = 0x2000;
    outer_scope.package_base = 0x33250800;
    outer_scope.member_table_offset = 0x24;
    outer_scope.object_fields_valid = true;
    outer_scope.stream_valid = true;
    outer_scope.caller_rva = 0x9FA980;
    const auto outer = context.Begin(outer_scope, 41);
    Require(outer.status == ResourceIdentityBeginStatus::tracked &&
                outer.token.tracked && outer.token.nesting_depth == 1,
            "outer resource identity scope was not tracked");

    ResourceIdentityReadInput read;
    read.stream = 0x2100;
    read.destination = 0x3000;
    read.caller_rva = 0x9FF0D2;
    read.requested = 4096;
    read.returned = 4096;
    read.hash_valid = true;
    read.sha256[0] = 0x9F;

    ResourceIdentitySample sample;
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::recorded,
            "matching outer resource stream was not correlated");
    Require(sample.sequence == 41 && sample.nesting_depth == 1 &&
                sample.read_ordinal == 1 && sample.scope.owner == 0x1000 &&
                sample.scope.argument4 == 0x1300 &&
                sample.scope.object_fields_valid && sample.scope.stream_valid &&
                sample.scope.stream == 0x2000 &&
                sample.read.stream == 0x2100 &&
                sample.read.caller_rva == 0x9FF0D2 &&
                sample.read.sha256[0] == 0x9F,
            "outer resource identity sample was corrupted");
    Require(IsResourceIdentitySampleUsable(
                sample, 0x9FA980, 0x9FF0D2),
            "valid resource identity sample failed the final gate");
    auto unusable = sample;
    unusable.scope.stream = 0;
    Require(!IsResourceIdentitySampleUsable(
                unusable, 0x9FA980, 0x9FF0D2),
            "null package stream passed the final identity gate");
    unusable = sample;
    unusable.read.stream = 0;
    Require(!IsResourceIdentitySampleUsable(
                unusable, 0x9FA980, 0x9FF0D2),
            "null inner stream passed the final identity gate");
    unusable = sample;
    unusable.scope.member_table_offset = -1;
    Require(!IsResourceIdentitySampleUsable(
                unusable, 0x9FA980, 0x9FF0D2),
            "negative member table offset passed the final identity gate");
    unusable = sample;
    unusable.scope.caller_rva = 0x9FA981;
    Require(!IsResourceIdentitySampleUsable(
                unusable, 0x9FA980, 0x9FF0D2),
            "wrong outer call site passed the final identity gate");
    unusable = sample;
    unusable.read.caller_rva = 0x9FF0D3;
    Require(!IsResourceIdentitySampleUsable(
                unusable, 0x9FA980, 0x9FF0D2),
            "wrong inner call site passed the final identity gate");
    unusable = sample;
    unusable.read_ordinal = 0;
    Require(!IsResourceIdentitySampleUsable(
                unusable, 0x9FA980, 0x9FF0D2),
            "zero ordinal passed the final identity gate");

    ResourceIdentityReadInput invalid_read_stream = read;
    invalid_read_stream.stream = 0;
    Require(context.ObserveRead(invalid_read_stream, sample) ==
                ResourceIdentityObserveStatus::invalid_read_stream,
            "null inner read stream was correlated");
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::recorded &&
                sample.read_ordinal == 2,
            "rejected null stream consumed a resource ordinal");

    ResourceIdentityScopeInput inner_scope = outer_scope;
    inner_scope.owner = 0x4000;
    const auto inner = context.Begin(inner_scope, 42);
    Require(inner.status == ResourceIdentityBeginStatus::tracked &&
                inner.token.nesting_depth == 2,
            "nested resource identity scope was not tracked");
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::recorded &&
                sample.sequence == 42 && sample.nesting_depth == 2 &&
                sample.read_ordinal == 1 && sample.scope.owner == 0x4000,
            "nested resource read did not use the innermost scope");
    Require(context.End(inner.token) == ResourceIdentityEndStatus::matched,
            "nested resource identity scope did not unwind");
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::recorded &&
                sample.sequence == 41 && sample.read_ordinal == 3,
            "outer resource scope did not resume after nesting");
    Require(context.End(outer.token) == ResourceIdentityEndStatus::matched &&
                context.Empty(),
            "outer resource identity scope did not unwind");
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::no_active_scope,
            "read without a resource scope was correlated");

    std::array<ResourceIdentityScopeToken, kResourceIdentityMaxDepth> tokens{};
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto result = context.Begin(outer_scope, 100 + index);
        Require(result.status == ResourceIdentityBeginStatus::tracked,
                "resource identity depth limit rejected a valid frame");
        tokens[index] = result.token;
    }
    const auto overflow = context.Begin(outer_scope, 999);
    Require(overflow.status == ResourceIdentityBeginStatus::suppressed_overflow &&
                context.SuppressedDepth() == 1,
            "resource identity depth overflow was not suppressed");
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::suppressed_overflow,
            "read inside an untracked overflow scope was misattributed");
    Require(context.End(overflow.token) ==
                ResourceIdentityEndStatus::suppressed_overflow_unwound,
            "overflow scope did not unwind");
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::recorded &&
                sample.sequence == 100 + tokens.size() - 1,
            "tracked scope did not resume after overflow");
    for (std::size_t index = tokens.size(); index != 0; --index) {
        Require(context.End(tokens[index - 1]) == ResourceIdentityEndStatus::matched,
                "tracked resource identity stack did not unwind in LIFO order");
    }
    Require(context.Empty(), "resource identity context remained active after unwind");

    ResourceIdentityScopeInput invalid_stream_scope = outer_scope;
    invalid_stream_scope.stream_valid = false;
    const auto invalid_stream = context.Begin(invalid_stream_scope, 450);
    Require(invalid_stream.status == ResourceIdentityBeginStatus::tracked,
            "invalid-stream scope setup failed");
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::invalid_scope_stream,
            "read from an invalid resource scope stream was correlated");
    invalid_stream_scope.stream_valid = true;
    invalid_stream_scope.stream = 0;
    Require(context.End(invalid_stream.token) == ResourceIdentityEndStatus::matched,
            "invalid-stream scope did not unwind");
    const auto null_stream = context.Begin(invalid_stream_scope, 451);
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::invalid_scope_stream,
            "read from a null resource scope stream was correlated");
    Require(context.End(null_stream.token) == ResourceIdentityEndStatus::matched,
            "null-stream scope did not unwind");

    const auto mismatched = context.Begin(outer_scope, 500);
    ResourceIdentityScopeToken wrong = mismatched.token;
    wrong.sequence = 501;
    Require(context.End(wrong) == ResourceIdentityEndStatus::context_reset &&
                context.Empty(),
            "mismatched resource scope did not fail closed");

    const auto incomplete = context.Begin(outer_scope, 600);
    Require(incomplete.token.tracked && !context.Empty(),
            "incomplete resource scope setup failed");
    context.Reset();
    Require(context.Empty() &&
                context.ObserveRead(read, sample) ==
                    ResourceIdentityObserveStatus::no_active_scope,
            "explicit reset did not discard an incomplete sequence");

    const auto bounded = context.Begin(outer_scope, 700);
    for (std::uint32_t ordinal = 1;
         ordinal <= kResourceIdentityMaxReadsPerScope;
         ++ordinal) {
        Require(context.ObserveRead(read, sample) ==
                    ResourceIdentityObserveStatus::recorded &&
                    sample.read_ordinal == ordinal,
                "resource read ordinal was not monotonic");
    }
    Require(context.ObserveRead(read, sample) ==
                ResourceIdentityObserveStatus::read_limit_reached,
            "resource identity read limit was not enforced");
    Require(context.End(bounded.token) == ResourceIdentityEndStatus::matched,
            "bounded resource scope did not unwind");
}

void TestPackageIdentityCatalog(const std::filesystem::path& root) {
    constexpr std::uint64_t segment_offset = 0x100;
    const auto game = root / L"package-catalog-game";
    const auto media = game / L"media";
    std::error_code directory_error;
    Require(std::filesystem::create_directories(media, directory_error) &&
                !directory_error,
            "cannot create package catalog fixture directories");

    auto obpk = MakeSyntheticNamedObpk();
    std::vector<std::byte> package(static_cast<std::size_t>(segment_offset));
    package.insert(package.end(), obpk.bytes.begin(), obpk.bytes.end());
    const auto manifest = MakeSyntheticManifest(segment_offset);
    WriteBytes(media / L"manifest.bin", manifest);
    WriteBytes(media / L"media.upak", package);

    const auto canonical = [](const std::wstring_view value) {
        auto normalized = NormalizeVirtualPath(value);
        Require(static_cast<bool>(normalized),
                "package catalog test path did not normalize");
        return std::move(*normalized.path);
    };
    std::array<CanonicalVirtualPath, 4> requested{
        canonical(L"media/ui/fixture/first.dds"),
        canonical(L"media/ui/fixture/second.dds"),
        canonical(L"media/ui/fixture/missing.dds"),
        canonical(L"media/no_segment/absent.dds"),
    };

    const auto built = BuildMediaPackageIdentityCatalog(game, requested);
    Require(static_cast<bool>(built), "synthetic package identity catalog failed");
    Require(built.snapshot->Entries().size() == 2,
            "synthetic package catalog mapped the wrong member count");
    Require(HasPackageIssue(
                built, PackageIdentityCatalogIssueCode::member_not_found) &&
                HasPackageIssue(
                    built, PackageIdentityCatalogIssueCode::path_not_in_manifest) &&
                built.dropped_issue_count == 0,
            "synthetic package catalog did not report unresolved paths");

    const PackageResourceIdentity first_identity{
        segment_offset, obpk.payload_offset, 1};
    const auto* const first = built.snapshot->Find(first_identity);
    Require(first != nullptr &&
                first->virtual_path.key == requested[0].key &&
                first->uncompressed_offset == 0 &&
                first->original_size == 144 && first->type_id == 6,
            "first synthetic package identity mapping is wrong");
    const auto* const second = built.snapshot->Find(requested[1]);
    Require(second != nullptr &&
                second->identity == PackageResourceIdentity{
                    segment_offset, obpk.payload_offset, 2} &&
                second->uncompressed_offset == 144 &&
                second->original_size == 144,
            "second synthetic package identity mapping is wrong");
    Require(built.snapshot->Find(PackageResourceIdentity{
                segment_offset, obpk.payload_offset, 3}) == nullptr &&
                built.snapshot->Find(L"media/ui/fixture/unknown.dds") == nullptr,
            "unknown synthetic package identity was resolved");

    PackageIdentityCatalogOptions limited;
    limited.max_requested_paths = 1;
    const auto limit_result = BuildMediaPackageIdentityCatalog(
        game, requested, limited);
    Require(!limit_result &&
                limit_result.error == PackageIdentityCatalogError::invalid_argument,
            "package catalog request limit did not fail closed");

    const auto unknown_type_obpk = MakeSyntheticNamedObpk(false, 42);
    std::vector<std::byte> unknown_type_package(
        static_cast<std::size_t>(segment_offset));
    unknown_type_package.insert(
        unknown_type_package.end(),
        unknown_type_obpk.bytes.begin(), unknown_type_obpk.bytes.end());
    WriteBytes(media / L"media.upak", unknown_type_package);
    std::array<CanonicalVirtualPath, 2> unknown_type_requested{
        canonical(L"media/ui/fixture/first.42"),
        canonical(L"media/ui/fixture/second.42"),
    };
    const auto unknown_type = BuildMediaPackageIdentityCatalog(
        game, unknown_type_requested);
    Require(unknown_type && unknown_type.issues.empty() &&
                unknown_type.snapshot->Entries().size() == 2 &&
                unknown_type.snapshot->Find(unknown_type_requested[0]) != nullptr &&
                unknown_type.snapshot->Find(unknown_type_requested[0])->type_id == 42,
            "unknown OBPK type did not receive its numeric extension");

    const auto block_obpk = MakeSyntheticNamedObpk(true);
    std::vector<std::byte> unsupported_package(
        static_cast<std::size_t>(segment_offset));
    unsupported_package.insert(
        unsupported_package.end(), block_obpk.bytes.begin(), block_obpk.bytes.end());
    WriteBytes(media / L"media.upak", unsupported_package);
    const auto unsupported = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(unsupported && unsupported.snapshot->Entries().empty() &&
                HasPackageIssue(
                    unsupported,
                    PackageIdentityCatalogIssueCode::unsupported_layout),
            "unsupported OBPK layout did not fall back safely");

    auto colliding_block_sizes = unsupported_package;
    PutU32(
        colliding_block_sizes,
        static_cast<std::size_t>(segment_offset) + block_obpk.names_end,
        block_obpk.payload_offset);
    PutU32(
        colliding_block_sizes,
        static_cast<std::size_t>(segment_offset) + block_obpk.names_end + 8,
        block_obpk.payload_offset + 8);
    PutU32(
        colliding_block_sizes,
        static_cast<std::size_t>(segment_offset) + block_obpk.payload_offset,
        block_obpk.payload_offset);
    PutU32(
        colliding_block_sizes,
        static_cast<std::size_t>(segment_offset) + block_obpk.payload_offset + 8,
        block_obpk.payload_offset + 8);
    WriteBytes(media / L"media.upak", colliding_block_sizes);
    const auto colliding_layout = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(colliding_layout && colliding_layout.snapshot->Entries().empty() &&
                HasPackageIssue(
                    colliding_layout,
                    PackageIdentityCatalogIssueCode::unsupported_layout),
            "block OBPK whose META sizes equal offsets was mistaken for stream");

    const auto empty_obpk = MakeSyntheticEmptyObpk();
    std::vector<std::byte> empty_package(static_cast<std::size_t>(segment_offset));
    empty_package.insert(
        empty_package.end(), empty_obpk.bytes.begin(), empty_obpk.bytes.end());
    WriteBytes(media / L"media.upak", empty_package);
    const auto no_members = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(no_members && no_members.snapshot->Entries().empty() &&
                HasPackageIssue(
                    no_members,
                    PackageIdentityCatalogIssueCode::member_not_found),
            "empty OBPK segment did not fall back safely");

    auto inconsistent_stream = package;
    PutU32(
        inconsistent_stream,
        static_cast<std::size_t>(segment_offset) + obpk.payload_offset,
        287);
    WriteBytes(media / L"media.upak", inconsistent_stream);
    const auto inconsistent = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!inconsistent &&
                inconsistent.error == PackageIdentityCatalogError::invalid_package,
            "inconsistent single-stream OBPK size was accepted");

    auto empty_stream_payload = package;
    empty_stream_payload.resize(
        static_cast<std::size_t>(segment_offset) + obpk.payload_offset +
        sizeof(std::uint32_t));
    WriteBytes(media / L"media.upak", empty_stream_payload);
    const auto no_compressed_bytes = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!no_compressed_bytes &&
                no_compressed_bytes.error ==
                    PackageIdentityCatalogError::invalid_package,
            "single-stream OBPK without compressed bytes was accepted");

    auto unknown_version = package;
    PutU32(
        unknown_version,
        static_cast<std::size_t>(segment_offset) + 5,
        10);
    WriteBytes(media / L"media.upak", unknown_version);
    const auto unsupported_version = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!unsupported_version &&
                unsupported_version.error ==
                    PackageIdentityCatalogError::invalid_package,
            "unknown OBPK version was accepted");

    auto overlapping_table = package;
    PutU32(overlapping_table, static_cast<std::size_t>(segment_offset) + 9, 0x1D);
    PutU32(
        overlapping_table,
        static_cast<std::size_t>(segment_offset) + 13,
        obpk.names_end - 0x1D);
    WriteBytes(media / L"media.upak", overlapping_table);
    const auto overlapping = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!overlapping &&
                overlapping.error == PackageIdentityCatalogError::invalid_package,
            "OBPK table overlapping its header was accepted");

    auto false_name_flag = package;
    false_name_flag[static_cast<std::size_t>(segment_offset) +
                    obpk.names_flag_offset] = std::byte{0};
    WriteBytes(media / L"media.upak", false_name_flag);
    const auto inconsistent_names = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!inconsistent_names &&
                inconsistent_names.error ==
                    PackageIdentityCatalogError::invalid_package,
            "OBPK names were accepted while their presence flag was clear");

    auto invalid_utf8_name = package;
    invalid_utf8_name[static_cast<std::size_t>(segment_offset) +
                      obpk.first_name_offset] = std::byte{0xFF};
    WriteBytes(media / L"media.upak", invalid_utf8_name);
    const auto invalid_name = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!invalid_name &&
                invalid_name.error == PackageIdentityCatalogError::invalid_package,
            "invalid UTF-8 OBPK member name was silently omitted");

    auto truncated_blocks = unsupported_package;
    truncated_blocks.resize(
        static_cast<std::size_t>(segment_offset) +
        block_obpk.payload_offset + sizeof(std::uint32_t));
    WriteBytes(media / L"media.upak", truncated_blocks);
    const auto truncated_block = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!truncated_block &&
                truncated_block.error == PackageIdentityCatalogError::invalid_package,
            "truncated per-member OBPK block table was treated as supported data");

    auto corrupt_package = package;
    corrupt_package[static_cast<std::size_t>(segment_offset)] = std::byte{0};
    WriteBytes(media / L"media.upak", corrupt_package);
    const auto corrupt = BuildMediaPackageIdentityCatalog(
        game, std::span<const CanonicalVirtualPath>(requested.data(), 2));
    Require(!corrupt &&
                corrupt.error == PackageIdentityCatalogError::invalid_package,
            "corrupt requested OBPK segment was accepted");

    const auto empty = BuildMediaPackageIdentityCatalog(
        root / L"does-not-exist",
        std::span<const CanonicalVirtualPath>{});
    Require(empty && empty.snapshot->Entries().empty(),
            "empty package catalog unnecessarily touched the game files");
}

void TestPackageDdsContractCatalog(const std::filesystem::path& root) {
    constexpr std::uint64_t segment_offset = 0x100;
    const auto game = root / L"package-dds-contract-game";
    const auto media = game / L"media";
    std::filesystem::create_directories(media);

    auto first_bytes = MakeDxt5Dds4x4();
    auto second_bytes = MakeDxt5Dds(8, 4);
    first_bytes.back() = std::byte{0xA5};
    second_bytes.back() = std::byte{0x5A};
    auto obpk = MakeSyntheticDdsStreamObpk({first_bytes, second_bytes});

    const auto write_package = [&](const SyntheticObpk& source) {
        std::vector<std::byte> package(
            static_cast<std::size_t>(segment_offset));
        package.insert(
            package.end(), source.bytes.begin(), source.bytes.end());
        WriteBytes(media / L"media.upak", package);
        WriteBytes(
            media / L"manifest.bin",
            MakeSyntheticManifest(segment_offset));
    };
    write_package(obpk);

    const auto canonical = [](const std::wstring_view value) {
        auto normalized = NormalizeVirtualPath(value);
        Require(static_cast<bool>(normalized),
                "DDS contract test path did not normalize");
        return std::move(*normalized.path);
    };
    std::array<CanonicalVirtualPath, 2> requested{
        canonical(L"media/ui/fixture/first.dds"),
        canonical(L"media/ui/fixture/second.dds")};
    const auto package_catalog = BuildMediaPackageIdentityCatalog(
        game, requested);
    Require(package_catalog && package_catalog.issues.empty() &&
                package_catalog.snapshot->Entries().size() == 2,
            "DDS contract identity fixture did not map");

    const auto contracts = BuildMediaPackageDdsContractCatalog(
        game, package_catalog.snapshot.get());
    Require(contracts && contracts.issues.empty() &&
                contracts.snapshot->Entries().size() == 2,
            "synthetic original DDS contracts did not build");
    const PackageResourceIdentity first_identity{
        segment_offset, obpk.payload_offset, 1};
    const PackageResourceIdentity second_identity{
        segment_offset, obpk.payload_offset, 2};
    const auto* const first = contracts.snapshot->Find(first_identity);
    const auto* const second = contracts.snapshot->Find(second_identity);
    const auto expected_first_full = ComputeSha256(first_bytes);
    const auto expected_first_payload = ComputeSha256(
        std::span(first_bytes).subspan(128));
    Require(first != nullptr && second != nullptr &&
                first->virtual_path.key == requested[0].key &&
                first->dds.width == 4 && first->dds.height == 4 &&
                first->dds.format == DdsFormat::bc3 &&
                first->dds.payload_size == 16 &&
                first->full_sha256 == expected_first_full.digest &&
                first->payload_sha256 == expected_first_payload.digest &&
                second->dds.width == 8 && second->dds.height == 4 &&
                second->dds.expected_file_size == second_bytes.size(),
            "synthetic original DDS contract content is wrong");

    PackageDdsContractCatalogOptions asset_limit;
    asset_limit.max_asset_bytes = first_bytes.size() - 1;
    const auto limited_assets = BuildMediaPackageDdsContractCatalog(
        game, package_catalog.snapshot.get(), asset_limit);
    Require(limited_assets && limited_assets.snapshot->Entries().empty() &&
                HasPackageDdsContractIssue(
                    limited_assets,
                    PackageDdsContractCatalogIssueCode::asset_too_large),
            "original DDS per-asset limit did not omit candidates");

    PackageDdsContractCatalogOptions stream_limit;
    stream_limit.max_uncompressed_stream_bytes = 1;
    const auto limited_stream = BuildMediaPackageDdsContractCatalog(
        game, package_catalog.snapshot.get(), stream_limit);
    Require(limited_stream && limited_stream.snapshot->Entries().empty() &&
                HasPackageDdsContractIssue(
                    limited_stream,
                    PackageDdsContractCatalogIssueCode::stream_too_large),
            "original DDS stream limit did not omit candidates");

    auto invalid_first = first_bytes;
    invalid_first[0] = std::byte{0};
    const auto invalid_obpk = MakeSyntheticDdsStreamObpk(
        {invalid_first, second_bytes});
    write_package(invalid_obpk);
    const auto invalid_catalog = BuildMediaPackageIdentityCatalog(
        game, requested);
    const auto invalid_contracts = BuildMediaPackageDdsContractCatalog(
        game, invalid_catalog.snapshot.get());
    Require(invalid_contracts &&
                invalid_contracts.snapshot->Entries().size() == 1 &&
                invalid_contracts.snapshot->Find(second_identity) != nullptr &&
                HasPackageDdsContractIssue(
                    invalid_contracts,
                    PackageDdsContractCatalogIssueCode::invalid_original_dds),
            "invalid original DDS was not rejected independently");

    auto corrupt_package = obpk;
    corrupt_package.bytes.pop_back();
    write_package(corrupt_package);
    const auto corrupt_catalog = BuildMediaPackageIdentityCatalog(
        game, requested);
    const auto corrupt_contracts = BuildMediaPackageDdsContractCatalog(
        game, corrupt_catalog.snapshot.get());
    Require(!corrupt_contracts &&
                corrupt_contracts.error ==
                    PackageDdsContractCatalogError::decompression_failed,
            "truncated original DDS zlib stream was accepted");

    const auto invalid_arguments = BuildMediaPackageDdsContractCatalog(
        game, nullptr);
    Require(!invalid_arguments &&
                invalid_arguments.error ==
                    PackageDdsContractCatalogError::invalid_argument,
            "null original DDS package catalog was accepted");
}

void TestLogger(const std::filesystem::path& root) {
    auto opened = OpenSessionLogger(root / L"logs", L"offline-test");
    Require(static_cast<bool>(opened), "logger open failed");
    Require(opened.logger->Write(LogLevel::info, L"Unicode: M\u00e9xico \U0001F3AE\nforged"),
            "logger write failed");
    std::ifstream input(opened.logger->Path(), std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)), {});
    Require(content.find("M\xC3\xA9xico \xF0\x9F\x8E\xAE") != std::string::npos,
            "logger output is not UTF-8");
    Require(content.find("\\nforged") != std::string::npos,
            "logger did not escape embedded newline");
    const std::wstring large_message(1024 * 1024, L'x');
    bool capped = false;
    for (int index = 0; index < 17; ++index) {
        if (!opened.logger->Write(LogLevel::info, large_message)) { capped = true; break; }
    }
    Require(capped && std::filesystem::file_size(opened.logger->Path()) <= 16 * 1024 * 1024,
            "session log exceeded its byte budget");
}

void TestModIndex(const std::filesystem::path& root) {
    const auto mods = root / L"mods";
    const auto alpha = mods / L"aaa_mod" / L"media" / L"shared";
    const auto first = mods / L"first_test_mod" / L"media" / L"ui";
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(first);

    const std::array<std::byte, 1> alpha_byte{static_cast<std::byte>(0xA1)};
    const std::array<std::byte, 1> first_byte{static_cast<std::byte>(0xF1)};
    WriteBytes(alpha / L"winner.bin", alpha_byte);
    std::filesystem::create_directories(first / L".." / L"shared");
    WriteBytes(first / L".." / L"shared" / L"winner.bin", first_byte);
    WriteBytes(first / L"icon.dds", MakeDxt5Dds4x4());
    const std::array<std::byte, 4> invalid_dds{};
    WriteBytes(first / L"bad.dds", invalid_dds);

    const auto built = BuildModIndex(mods);
    Require(static_cast<bool>(built), "mod index build failed");
    Require(built.snapshot->ModIds().size() == 2, "mod count mismatch");
    Require(built.snapshot->ModIds()[0] == L"aaa_mod", "mod priority is not lexical");
    Require(built.snapshot->ModIds()[1] == L"first_test_mod", "first_test_mod was not indexed");

    const auto* winner = built.snapshot->Find(L"MEDIA/shared/WINNER.bin");
    Require(winner != nullptr && winner->mod_id == L"aaa_mod", "priority winner mismatch");
    Require(winner->storage->Bytes()[0] == static_cast<std::byte>(0xA1), "winner bytes mismatch");
    const auto* icon = built.snapshot->Find(L"media/ui/icon.dds");
    Require(icon != nullptr && icon->mod_id == L"first_test_mod", "valid DDS not indexed");
    Require(icon->dds && icon->dds->format == DdsFormat::bc3, "DDS metadata absent");
    Require(built.snapshot->Find(L"media/ui/bad.dds") == nullptr, "invalid DDS indexed");

    bool saw_invalid_dds = false;
    for (const auto& issue : built.issues) {
        saw_invalid_dds = saw_invalid_dds || issue.code == ModIndexIssueCode::invalid_dds;
    }
    Require(saw_invalid_dds, "invalid DDS rejection was not reported");

    const auto empty = BuildModIndex(root / L"missing-mods-root");
    Require(empty && empty.snapshot->Assets().empty(), "missing mods root should be an empty snapshot");
}

void TestGeneralDdsCandidates(const std::filesystem::path& root) {
    constexpr std::uint64_t segment_offset = 0x100;
    const auto game = root / L"general-dds-game";
    const auto media = game / L"media";
    std::filesystem::create_directories(media);

    const auto write_package = [&](const SyntheticObpk& obpk) {
        std::vector<std::byte> package(static_cast<std::size_t>(segment_offset));
        package.insert(package.end(), obpk.bytes.begin(), obpk.bytes.end());
        WriteBytes(media / L"media.upak", package);
    };
    WriteBytes(media / L"manifest.bin", MakeSyntheticManifest(segment_offset));
    auto original_first = MakeDxt5Dds4x4();
    auto original_second = MakeDxt5Dds4x4();
    original_first.back() = std::byte{0x11};
    original_second.back() = std::byte{0x22};
    const auto stream_obpk = MakeSyntheticDdsStreamObpk(
        {original_first, original_second});
    write_package(stream_obpk);

    const auto canonical = [](const std::wstring_view value) {
        auto normalized = NormalizeVirtualPath(value);
        Require(static_cast<bool>(normalized),
                "general DDS candidate path did not normalize");
        return std::move(*normalized.path);
    };
    std::array<CanonicalVirtualPath, 2> requested{
        canonical(L"media/ui/fixture/first.dds"),
        canonical(L"media/ui/fixture/second.dds"),
    };
    auto catalog = BuildMediaPackageIdentityCatalog(game, requested);
    Require(catalog && catalog.issues.empty() &&
                catalog.snapshot->Entries().size() == 2,
            "general DDS candidate package catalog failed");
    auto contracts = BuildMediaPackageDdsContractCatalog(
        game, catalog.snapshot.get());
    Require(contracts && contracts.issues.empty() &&
                contracts.snapshot->Entries().size() == 2,
            "general DDS original contracts failed");

    const auto empty_mods = BuildModIndex(root / L"general-dds-empty-mods");
    Require(static_cast<bool>(empty_mods),
            "general DDS empty mod index failed");
    GeneralDdsCandidateOptions one_issue;
    one_issue.max_issue_count = 1;
    const auto bounded = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), contracts.snapshot.get(),
        empty_mods.snapshot.get(), one_issue);
    Require(bounded && bounded.snapshot->Entries().empty() &&
                bounded.issues.size() == 1 &&
                bounded.dropped_issue_count == 1 &&
                bounded.issues[0].code ==
                    GeneralDdsCandidateIssueCode::mod_asset_not_found,
            "general DDS candidate rejection diagnostics were not bounded");

    const auto null_catalog = BuildGeneralDdsCandidateSnapshot(
        nullptr, contracts.snapshot.get(), empty_mods.snapshot.get());
    const auto null_contracts = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), nullptr, empty_mods.snapshot.get());
    const auto null_index = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), contracts.snapshot.get(), nullptr);
    Require(!null_catalog && !null_contracts && !null_index &&
                null_catalog.error == GeneralDdsCandidateError::invalid_argument &&
                null_contracts.error == GeneralDdsCandidateError::invalid_argument &&
                null_index.error == GeneralDdsCandidateError::invalid_argument,
            "general DDS candidate null inputs did not fail globally");

    const auto mods = root / L"general-dds-mods";
    const auto alpha = mods / L"aaa_mod" / L"media" / L"ui" / L"fixture";
    const auto omega = mods / L"zzz_mod" / L"media" / L"ui" / L"fixture";
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(omega);

    const std::array<std::byte, 4> not_dds{};
    WriteBytes(alpha / L"first.dds", not_dds);
    const auto invalid_dds_index = BuildModIndex(mods);
    Require(invalid_dds_index &&
                HasIssue(invalid_dds_index, ModIndexIssueCode::invalid_dds),
            "non-DDS candidate fixture was not rejected by the mod index");
    const auto invalid_dds_join = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), contracts.snapshot.get(),
        invalid_dds_index.snapshot.get());
    Require(invalid_dds_join &&
                invalid_dds_join.snapshot->Entries().empty() &&
                HasGeneralDdsIssue(
                    invalid_dds_join,
                    GeneralDdsCandidateIssueCode::mod_asset_not_found),
            "non-DDS loose asset reached the general candidate snapshot");

    auto alpha_first = MakeDxt5Dds4x4();
    alpha_first.back() = std::byte{0xA1};
    auto omega_first = MakeDxt5Dds4x4();
    omega_first.back() = std::byte{0xF1};
    WriteBytes(alpha / L"first.dds", alpha_first);
    WriteBytes(omega / L"first.dds", omega_first);

    const auto one_asset_index = BuildModIndex(mods);
    Require(static_cast<bool>(one_asset_index),
            "single general DDS mod index failed");
    const auto one_candidate = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), contracts.snapshot.get(),
        one_asset_index.snapshot.get());
    Require(one_candidate && one_candidate.snapshot->Entries().size() == 1 &&
                HasGeneralDdsIssue(
                    one_candidate,
                    GeneralDdsCandidateIssueCode::mod_asset_not_found),
            "missing loose DDS was not omitted independently");
    const auto first_identity = PackageResourceIdentity{
        segment_offset, stream_obpk.payload_offset, 1};
    const auto second_identity = PackageResourceIdentity{
        segment_offset, stream_obpk.payload_offset, 2};
    const auto* const priority_winner =
        one_candidate.snapshot->Find(first_identity);
    Require(priority_winner != nullptr &&
                priority_winner->mod_id == L"aaa_mod" &&
                priority_winner->storage->Bytes().back() == std::byte{0xA1},
            "general DDS snapshot did not preserve the priority winner");

    WriteBytes(alpha / L"second.dds", MakeDxt5Dds(8, 4));
    const auto mismatch_index = BuildModIndex(mods);
    Require(static_cast<bool>(mismatch_index),
            "mismatched general DDS mod index failed");
    const auto mismatch = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), contracts.snapshot.get(),
        mismatch_index.snapshot.get());
    Require(mismatch && mismatch.snapshot->Entries().size() == 1 &&
                HasGeneralDdsIssue(
                    mismatch,
                    GeneralDdsCandidateIssueCode::original_size_mismatch),
            "size-mismatched DDS reached the general candidate snapshot");

    const auto same_size_wrong_contract = MakeDxt1Dds(8, 4);
    Require(same_size_wrong_contract.size() == original_second.size(),
            "same-size DDS contract regression fixture is not the same size");
    WriteBytes(alpha / L"second.dds", same_size_wrong_contract);
    const auto wrong_contract_index = BuildModIndex(mods);
    Require(static_cast<bool>(wrong_contract_index),
            "same-size wrong-contract mod index failed");
    const auto wrong_contract = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), contracts.snapshot.get(),
        wrong_contract_index.snapshot.get());
    Require(wrong_contract &&
                wrong_contract.snapshot->Entries().size() == 1 &&
                HasGeneralDdsIssue(
                    wrong_contract,
                    GeneralDdsCandidateIssueCode::original_contract_mismatch),
            "same-size DDS with different dimensions/format reached the general snapshot");

    WriteBytes(alpha / L"second.dds", MakeDxt5Dds4x4());
    auto complete_index = BuildModIndex(mods);
    Require(static_cast<bool>(complete_index),
            "complete general DDS mod index failed");
    auto complete = BuildGeneralDdsCandidateSnapshot(
        catalog.snapshot.get(), contracts.snapshot.get(),
        complete_index.snapshot.get());
    Require(complete && complete.issues.empty() &&
                complete.snapshot->Entries().size() == 2,
            "valid general DDS candidates did not join cleanly");
    const auto entries = complete.snapshot->Entries();
    Require(entries[0].identity == first_identity &&
                entries[1].identity == second_identity,
            "general DDS snapshot is not ordered by package identity");
    const auto* const first = complete.snapshot->Find(first_identity);
    Require(first != nullptr,
            "first general DDS candidate lookup failed");
    const auto expected_replacement_payload_hash = ComputeSha256(
        first->storage->Bytes().subspan(128));
    Require(static_cast<bool>(expected_replacement_payload_hash),
            "general DDS replacement payload hash fixture failed");
    Require(first->virtual_path.key == requested[0].key &&
                first->original_size == 144 &&
                first->dds.format == DdsFormat::bc3 &&
                first->dds.header_size == 128 &&
                first->payload_offset == 128 &&
                first->payload_size == 16 &&
                first->original_full_sha256 ==
                    contracts.snapshot->Find(first_identity)->full_sha256 &&
                first->original_payload_sha256 ==
                    contracts.snapshot->Find(first_identity)->payload_sha256 &&
                first->replacement_full_sha256 == first->storage->Sha256() &&
                first->replacement_payload_sha256 ==
                    expected_replacement_payload_hash.digest &&
                first->storage != nullptr,
            "general DDS candidate did not preserve its immutable contract");
    Require(complete.snapshot->Find(PackageResourceIdentity{
                segment_offset, stream_obpk.payload_offset, 3}) == nullptr,
            "unknown package identity resolved to a general DDS candidate");

    const auto retained_snapshot = complete.snapshot;
    complete.snapshot.reset();
    complete_index.snapshot.reset();
    catalog.snapshot.reset();
    contracts.snapshot.reset();
    Require(retained_snapshot->Find(first_identity) != nullptr &&
                retained_snapshot->Find(first_identity)->storage != nullptr &&
                retained_snapshot->Find(first_identity)->storage->Bytes().back() ==
                    std::byte{0xA1},
            "general DDS snapshot depended on its source snapshots' lifetimes");

    const auto evaluate = [&](const PackageResourceIdentity identity,
                              const bool destination_valid,
                              const std::int32_t requested_size,
                              const std::int32_t returned_size) {
        return EvaluateGeneralDdsDryRun(
            retained_snapshot.get(),
            [&] {
                GeneralDdsDryRunInput input{
                    identity, destination_valid, requested_size, returned_size};
                const auto* const candidate = retained_snapshot->Find(identity);
                if (candidate != nullptr && requested_size > 0) {
                    input.source_hash_valid = true;
                    input.source_sha256 = requested_size ==
                            static_cast<std::int32_t>(candidate->original_size)
                        ? candidate->original_full_sha256
                        : candidate->original_payload_sha256;
                }
                return input;
            }());
    };
    const auto full_evaluation = evaluate(first_identity, true, 144, 144);
#if defined(DS2_TEST_GENERAL_RESOLVER_RUNTIME)
    TestGeneralResolverRuntime(*retained_snapshot, *first, original_first);
#endif
    Require(full_evaluation.decision == GeneralDdsDryRunDecision::full_dds &&
                full_evaluation.candidate == first,
            "complete DDS read was not classified");
    const auto payload_evaluation = evaluate(first_identity, true, 16, 16);
    Require(payload_evaluation.decision == GeneralDdsDryRunDecision::payload,
            "DDS payload read was not classified");
    const auto full_replacement = GeneralDdsReplacementBytes(full_evaluation);
    const auto payload_replacement = GeneralDdsReplacementBytes(
        payload_evaluation);
    Require(full_replacement.size() == first->original_size &&
                full_replacement.data() == first->storage->Data() &&
                payload_replacement.size() == first->payload_size &&
                payload_replacement.data() ==
                    first->storage->Data() + first->payload_offset,
            "general DDS replacement byte selection is inconsistent");
    Require(GeneralDdsReplacementBytes(
                GeneralDdsDryRunEvaluation{
                    GeneralDdsDryRunDecision::short_read, first}).empty() &&
                GeneralDdsReplacementBytes(
                    GeneralDdsDryRunEvaluation{}).empty(),
            "rejected general DDS evaluation exposed replacement bytes");
    GeneralDdsDryRunInput wrong_hash_input{
        first_identity, true, 16, 16, true,
        first->original_payload_sha256};
    wrong_hash_input.source_sha256[0] ^= 0xFF;
    Require(EvaluateGeneralDdsDryRun(
                retained_snapshot.get(), wrong_hash_input).decision ==
                    GeneralDdsDryRunDecision::source_hash_mismatch,
            "unexpected original payload hash was accepted");
    Require(EvaluateGeneralDdsDryRun(
                retained_snapshot.get(),
                GeneralDdsDryRunInput{
                    first_identity, true, 16, 16}).decision ==
                    GeneralDdsDryRunDecision::invalid,
            "missing original payload hash was accepted");
    Require(evaluate(first_identity, true, 144, 100).decision ==
                GeneralDdsDryRunDecision::short_read,
            "short DDS read was not classified");
    Require(evaluate(first_identity, true, 32, 32).decision ==
                GeneralDdsDryRunDecision::size_mismatch &&
                evaluate(first_identity, true, 32, 16).decision ==
                    GeneralDdsDryRunDecision::size_mismatch,
            "unexpected DDS read size was not rejected");
    Require(evaluate(first_identity, false, 144, 144).decision ==
                GeneralDdsDryRunDecision::invalid &&
                evaluate(first_identity, true, -1, -1).decision ==
                    GeneralDdsDryRunDecision::invalid &&
                evaluate(first_identity, true, 16, 17).decision ==
                    GeneralDdsDryRunDecision::invalid,
            "invalid DDS read inputs were not rejected");
    Require(evaluate(
                PackageResourceIdentity{segment_offset, 0xDEAD, 1},
                true, 144, 144).decision ==
                    GeneralDdsDryRunDecision::unmapped &&
                EvaluateGeneralDdsDryRun(
                    nullptr,
                    GeneralDdsDryRunInput{
                        first_identity, true, 144, 144}).decision ==
                    GeneralDdsDryRunDecision::invalid,
            "unmapped or null-snapshot DDS read classification is wrong");

    const auto dx10_obpk = MakeSyntheticDdsStreamObpk(
        {MakeDx10Bc3Dds4x4(), MakeDxt5Dds4x4()});
    write_package(dx10_obpk);
    const auto dx10_catalog = BuildMediaPackageIdentityCatalog(game, requested);
    Require(dx10_catalog && dx10_catalog.issues.empty(),
            "DX10 exclusion package catalog failed");
    const auto dx10_contracts = BuildMediaPackageDdsContractCatalog(
        game, dx10_catalog.snapshot.get());
    Require(dx10_contracts && dx10_contracts.issues.empty(),
            "DX10 original package contract failed");
    WriteBytes(alpha / L"first.dds", MakeDx10Bc3Dds4x4());
    const auto dx10_index = BuildModIndex(mods);
    Require(dx10_index &&
                dx10_index.snapshot->Find(requested[0]) != nullptr &&
                dx10_index.snapshot->Find(requested[0])->dds.has_value() &&
                dx10_index.snapshot->Find(requested[0])->dds->has_dx10_header,
            "valid DX10 DDS fixture did not reach the mod index");
    const auto dx10_join = BuildGeneralDdsCandidateSnapshot(
        dx10_catalog.snapshot.get(), dx10_contracts.snapshot.get(),
        dx10_index.snapshot.get());
    Require(dx10_join && dx10_join.snapshot->Entries().size() == 1 &&
                HasGeneralDdsIssue(
                    dx10_join,
                    GeneralDdsCandidateIssueCode::dx10_header_not_supported),
            "DX10 DDS reached the traditional general candidate snapshot");

    const auto unknown_type_obpk = MakeSyntheticNamedObpk(false, 42);
    write_package(unknown_type_obpk);
    std::array<CanonicalVirtualPath, 2> unknown_type_requested{
        canonical(L"media/ui/fixture/first.42"),
        canonical(L"media/ui/fixture/second.42"),
    };
    const auto unknown_type_catalog = BuildMediaPackageIdentityCatalog(
        game, unknown_type_requested);
    Require(unknown_type_catalog && unknown_type_catalog.issues.empty(),
            "unknown-type general candidate catalog failed");
    const auto unknown_type_contracts = BuildMediaPackageDdsContractCatalog(
        game, unknown_type_catalog.snapshot.get());
    Require(unknown_type_contracts &&
                unknown_type_contracts.snapshot->Entries().empty() &&
                HasPackageDdsContractIssue(
                    unknown_type_contracts,
                    PackageDdsContractCatalogIssueCode::unsupported_type),
            "unknown package type reached should not produce a DDS contract");
    const auto unknown_type_join = BuildGeneralDdsCandidateSnapshot(
        unknown_type_catalog.snapshot.get(),
        unknown_type_contracts.snapshot.get(), dx10_index.snapshot.get());
    Require(unknown_type_join &&
                unknown_type_join.snapshot->Entries().empty() &&
                unknown_type_join.issues.size() == 2 &&
                HasGeneralDdsIssue(
                    unknown_type_join,
                    GeneralDdsCandidateIssueCode::unsupported_type),
            "non-DDS package type reached the general candidate snapshot");
}

void TestByteStorageContainment(const std::filesystem::path& root) {
    const auto confined_root = root / L"byte-storage-root";
    const auto sibling_root = root / L"byte-storage-root-sibling";
    std::filesystem::create_directories(confined_root);
    std::filesystem::create_directories(sibling_root);

    const std::array<std::byte, 2> inside_bytes{
        static_cast<std::byte>(0x11), static_cast<std::byte>(0x22)};
    const std::array<std::byte, 1> outside_bytes{static_cast<std::byte>(0x33)};
    const auto inside = confined_root / L"inside.bin";
    const auto outside = sibling_root / L"outside.bin";
    WriteBytes(inside, inside_bytes);
    WriteBytes(outside, outside_bytes);

    const auto exact = LoadByteStorageUnderRoot(confined_root, inside, inside_bytes.size());
    Require(exact && exact.storage->Size() == inside_bytes.size(),
            "contained asset failed its exact size limit");
    const auto too_small = LoadByteStorageUnderRoot(confined_root, inside, inside_bytes.size() - 1);
    Require(!too_small && too_small.error == ByteLoadError::file_too_large,
            "contained asset exceeded size limit without rejection");
    const auto escaped = LoadByteStorageUnderRoot(confined_root, outside, outside_bytes.size());
    Require(!escaped && escaped.error == ByteLoadError::outside_root,
            "sibling-prefix asset escaped the root handle check");
    const auto zero_limit = LoadByteStorageUnderRoot(confined_root, inside, 0);
    Require(!zero_limit && zero_limit.error == ByteLoadError::invalid_path,
            "zero byte-storage limit was accepted");
    const auto directory = LoadByteStorageUnderRoot(confined_root, confined_root, 1);
    Require(!directory,
            "directory was accepted as byte storage");
}

void RequireResourceLimitFailure(
    const ModIndexBuildResult& result,
    const char* const message) {
    Require(!result && result.snapshot == nullptr &&
                result.error == ModIndexError::resource_limit_exceeded,
            message);
}

void TestModIndexLimits(const std::filesystem::path& root) {
    const std::array<std::byte, 1> one_byte{static_cast<std::byte>(0x41)};
    const std::array<std::byte, 2> two_bytes{
        static_cast<std::byte>(0x41), static_cast<std::byte>(0x42)};

    const auto asset_root = root / L"asset-count-mods";
    std::filesystem::create_directories(asset_root / L"mod");
    WriteBytes(asset_root / L"mod" / L"one.bin", one_byte);
    WriteBytes(asset_root / L"mod" / L"two.bin", one_byte);
    ModIndexOptions asset_options;
    asset_options.max_asset_count = 1;
    const auto asset_limited = BuildModIndex(asset_root, asset_options);
    RequireResourceLimitFailure(asset_limited, "asset-count limit did not fail closed");
    Require(HasIssue(asset_limited, ModIndexIssueCode::resource_limit_exceeded),
            "asset-count failure was not reported");

    const auto byte_root = root / L"total-byte-mods";
    std::filesystem::create_directories(byte_root / L"mod");
    WriteBytes(byte_root / L"mod" / L"two.bin", two_bytes);
    ModIndexOptions byte_options;
    byte_options.max_total_bytes = 1;
    const auto byte_limited = BuildModIndex(byte_root, byte_options);
    RequireResourceLimitFailure(byte_limited, "aggregate-byte limit did not fail closed");

    const auto entry_root = root / L"entry-count-mods";
    std::filesystem::create_directories(entry_root / L"mod");
    WriteBytes(entry_root / L"mod" / L"asset.bin", one_byte);
    ModIndexOptions entry_options;
    entry_options.max_entry_count = 1;
    const auto entry_limited = BuildModIndex(entry_root, entry_options);
    RequireResourceLimitFailure(entry_limited, "entry-count limit did not fail closed");

    const auto depth_root = root / L"depth-mods";
    std::filesystem::create_directories(depth_root / L"mod" / L"level");
    WriteBytes(depth_root / L"mod" / L"level" / L"asset.bin", one_byte);
    ModIndexOptions depth_options;
    depth_options.max_depth = 1;
    const auto depth_limited = BuildModIndex(depth_root, depth_options);
    RequireResourceLimitFailure(depth_limited, "directory-depth limit did not fail closed");

    const auto issue_root = root / L"issue-count-mods";
    std::filesystem::create_directories(issue_root / L"mod");
    WriteBytes(issue_root / L"mod" / L"bad-one.dds", one_byte);
    WriteBytes(issue_root / L"mod" / L"bad-two.dds", one_byte);
    WriteBytes(issue_root / L"mod" / L"bad-three.dds", one_byte);
    ModIndexOptions issue_options;
    issue_options.max_issue_count = 1;
    const auto issue_limited = BuildModIndex(issue_root, issue_options);
    Require(issue_limited && issue_limited.issues.size() == 1 &&
                issue_limited.dropped_issue_count == 2,
            "issue cap or dropped counter mismatch");

    ModIndexOptions invalid_options;
    invalid_options.max_total_bytes = 0;
    const auto invalid = BuildModIndex(issue_root, invalid_options);
    Require(!invalid && invalid.error == ModIndexError::invalid_root,
            "zero aggregate-byte limit was accepted");
}

void TestCanonicalModIdCollision(const std::filesystem::path& root) {
    const auto mods = root / L"canonical-id-mods";
    const auto composed = mods / L"caf\u00e9";
    const auto decomposed = mods / L"cafe\u0301";
    Require(std::filesystem::create_directories(composed),
            "cannot create composed mod-id fixture");
    Require(std::filesystem::create_directories(decomposed),
            "cannot create decomposed mod-id fixture");

    const std::array<std::byte, 1> bytes{static_cast<std::byte>(0x51)};
    WriteBytes(composed / L"composed.bin", bytes);
    WriteBytes(decomposed / L"decomposed.bin", bytes);

    const auto built = BuildModIndex(mods);
    Require(built && built.snapshot->ModIds().empty() &&
                built.snapshot->Assets().empty(),
            "canonical mod-id collision did not reject the whole group");
    std::size_t duplicate_count = 0;
    for (const auto& issue : built.issues) {
        if (issue.code == ModIndexIssueCode::duplicate_mod_id) {
            ++duplicate_count;
        }
    }
    Require(duplicate_count == 2,
            "canonical mod-id collision was not reported for every alias");
}

void TestEditedAsset(const std::filesystem::path& asset_path) {
    const auto loaded = LoadByteStorage(asset_path, 4'224);
    Require(static_cast<bool>(loaded), "edited DDS could not be loaded");
    Require(loaded.storage->Sha256Hex() ==
                L"2C0D7E050F846A6337A33982FB221C0F38A0E265D5FACF45074B7D02EE9CB719",
            "edited DDS hash mismatch");
    DdsExpectations expected;
    expected.width = 64;
    expected.height = 64;
    expected.mip_count = 1;
    expected.format = DdsFormat::bc3;
    expected.exact_file_size = 4'224;
    Require(static_cast<bool>(ValidateDds(loaded.storage->Bytes(), expected)),
            "edited DDS contract mismatch");
}

void TestSupportedGame(const std::filesystem::path& executable_path) {
    const auto build = IdentifyGameBuild(executable_path);
    Require(build.Supported(), "installed Darksiders2.exe is not the supported build");
    Require(build.file_size == 33'637'376, "supported executable size mismatch");

    const auto image = LoadByteStorage(executable_path, 40ull * 1024ull * 1024ull);
    Require(static_cast<bool>(image),
            "supported executable could not be loaded for candidate signature scan");
    const auto pattern = ParseSignature(
        "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 55 53 56 57 "
        "41 54 41 55 41 56 41 57 48 8D 68 A8 48 81 EC 18 01 00 00");
    Require(static_cast<bool>(pattern),
            "resource identity candidate signature did not parse");
    const auto scan = ScanUnique(image.storage->Bytes(), pattern.pattern);
    Require(scan && scan.matches == 1,
            "resource identity candidate signature is not unique in the supported executable");
    const auto raw_offset = static_cast<std::size_t>(
        scan.address - image.storage->Data());
    // In this PE, .text starts at RVA 0x1000 and raw offset 0x400.
    Require(raw_offset == 0x009FE25C && raw_offset + 0xC00 == 0x009FEE5C,
            "resource identity candidate signature moved from the expected RVA");

    auto target_path = NormalizeVirtualPath(
        L"media/ui/ui_icons_small/"
        L"ui_hudicon_passiveability_improved_agility.dds");
    auto second_path = NormalizeVirtualPath(
        L"media/ui/ui_core/ui_icon_highlight.dds");
    auto different_size_path = NormalizeVirtualPath(
        L"media/ui/ui_core/icon_artifact.dds");
    Require(target_path && second_path && different_size_path,
            "real package identity paths did not normalize");
    std::array<CanonicalVirtualPath, 3> requested{
        std::move(*target_path.path),
        std::move(*second_path.path),
        std::move(*different_size_path.path),
    };
    const auto catalog = BuildMediaPackageIdentityCatalog(
        executable_path.parent_path(), requested);
    Require(catalog && catalog.issues.empty(),
            "installed media package identity catalog did not build cleanly");

    const auto* const target = catalog.snapshot->Find(requested[0]);
    Require(target != nullptr &&
                target->identity == PackageResourceIdentity{
                    0x33250800ull, 0x3000u, 73u} &&
                target->uncompressed_offset == 0x4F800 &&
                target->original_size == 0x1080 && target->type_id == 6,
            "first real package identity does not match static/dynamic evidence");
    const auto* const second = catalog.snapshot->Find(requested[1]);
    Require(second != nullptr &&
                second->identity == PackageResourceIdentity{
                    0x77800ull, 0x3800u, 46u} &&
                second->uncompressed_offset == 0x267E78 &&
                second->original_size == 0x1080 && second->type_id == 6,
            "second real package identity does not match static/dynamic evidence");
    const auto* const different_size = catalog.snapshot->Find(requested[2]);
    Require(different_size != nullptr &&
                different_size->identity == PackageResourceIdentity{
                    0x77800ull, 0x3800u, 143u} &&
                different_size->uncompressed_offset == 0xB727B8 &&
                different_size->original_size == 0x480 &&
                different_size->type_id == 6,
            "different-size real package identity does not match package evidence");

    const auto contracts = BuildMediaPackageDdsContractCatalog(
        executable_path.parent_path(), catalog.snapshot.get());
    Require(contracts && contracts.issues.empty() &&
                contracts.snapshot->Entries().size() == 3,
            "installed original DDS contract catalog did not build cleanly");
    const auto* const target_contract = contracts.snapshot->Find(target->identity);
    const auto* const second_contract = contracts.snapshot->Find(second->identity);
    const auto* const different_size_contract =
        contracts.snapshot->Find(different_size->identity);
    Require(target_contract != nullptr &&
                target_contract->dds.width == 64 &&
                target_contract->dds.height == 64 &&
                target_contract->dds.mip_count == 1 &&
                target_contract->dds.format == DdsFormat::bc3 &&
                Sha256EqualsHex(
                    target_contract->full_sha256,
                    L"E2173F05C575677756071AD7DEC88E4CF49BBD0A734F902159E93C79F191B22D") &&
                Sha256EqualsHex(
                    target_contract->payload_sha256,
                    L"9F008D044870B6412C2636191E52699B2D9E8427B09328730AAB5E8595BEEE51"),
            "first installed original DDS contract is wrong");
    Require(second_contract != nullptr &&
                second_contract->dds.width == 64 &&
                second_contract->dds.height == 64 &&
                second_contract->dds.mip_count == 1 &&
                second_contract->dds.format == DdsFormat::bc3 &&
                Sha256EqualsHex(
                    second_contract->full_sha256,
                    L"7D7CA1D2B1A411DA28BA4071BB6D0A9AC54FFB5F2C0E608B28333A2D34EA3254") &&
                Sha256EqualsHex(
                    second_contract->payload_sha256,
                    L"9726E7AB99C271F878EB57DA8681DB340F8F435C1208FB6132F7CB3736A9A8FA"),
            "second installed original DDS contract is wrong");
    Require(different_size_contract != nullptr &&
                different_size_contract->dds.width == 32 &&
                different_size_contract->dds.height == 32 &&
                different_size_contract->dds.mip_count == 1 &&
                different_size_contract->dds.format == DdsFormat::bc3 &&
                Sha256EqualsHex(
                    different_size_contract->full_sha256,
                    L"7BCD13C620C6249B7FB80E60B5237F42548C7D5F4EE10B40A04CABBC94E3CE10") &&
                Sha256EqualsHex(
                    different_size_contract->payload_sha256,
                    L"A1E8C36BBDDB387EAEF3F57BF8BEFA24FFDDF09C7C644FEEC3B42E4D1A13D2F7"),
            "different-size installed original DDS contract is wrong");

    auto blocked_path = NormalizeVirtualPath(
        L"media/ui/ui_icons_tips/ui_hudicon_goldskull_256.dds");
    auto empty_segment_path = NormalizeVirtualPath(
        L"media/world/environments/Zone_02/zone02_md3_rm01/absent.dds");
    Require(blocked_path && empty_segment_path,
            "real unsupported-layout paths did not normalize");
    std::array<CanonicalVirtualPath, 2> unsupported_requested{
        std::move(*blocked_path.path),
        std::move(*empty_segment_path.path),
    };
    const auto unsupported = BuildMediaPackageIdentityCatalog(
        executable_path.parent_path(), unsupported_requested);
    Require(unsupported && unsupported.snapshot->Entries().empty() &&
                HasPackageIssue(
                    unsupported,
                    PackageIdentityCatalogIssueCode::unsupported_layout) &&
                HasPackageIssue(
                    unsupported,
                    PackageIdentityCatalogIssueCode::member_not_found),
            "installed block/empty OBPK layouts did not fall back safely");
}

void TestDeployedMod(const std::filesystem::path& mods_root) {
    ModIndexOptions options;
    options.dds_only = true;
    const auto built = BuildModIndex(mods_root, options);
    Require(static_cast<bool>(built), "deployed mod index build failed");
    Require(!built.snapshot->Assets().empty(), "no deployed DDS indexed");
    for (const auto& asset : built.snapshot->Assets()) {
        Require(asset.dds.has_value() && asset.storage && asset.storage->Size() > 128,
                "deployed DDS has no validated storage");
    }
}

void TestLoaderConfig(const std::filesystem::path& root) {
    const auto absent = LoadLoaderConfig(root);
    Require(absent.valid && absent.config.enabled && absent.config.write_enabled,
            "missing config did not use defaults");
    const auto disabled = ParseLoaderConfig("[loader]\nenabled=false\nmode=observe\n");
    Require(disabled.valid && !disabled.config.enabled && !disabled.config.write_enabled,
            "disabled/observe config failed");
    Require(ParseLoaderConfig("; comment\r\n[loader]\r\nenabled = true\r\nmode = override\r\n").valid,
            "valid CRLF config rejected");
    for (const auto invalid : {"", "mode=override", "[other]", "[loader]\npath=..",
         "[loader]\nenabled=1", "[loader]\nmode=anything", "[loader]\nmode=observe\nmode=override",
         "[loader]\n[loader]", "[loader]\nenabled=true\nenabled=false"}) {
        Require(!ParseLoaderConfig(invalid).valid, "invalid config accepted");
    }
    Require(!ParseLoaderConfig(std::string(16385, ' ')).valid, "oversized config accepted");
    const std::array<std::byte, 1> nul{std::byte{0}};
    WriteBytes(root / L"Darksiders2DLL.ini", nul);
    Require(!LoadLoaderConfig(root).valid, "malformed config file accepted");
    std::filesystem::remove(root / L"Darksiders2DLL.ini");

    // Shipping indexing ignores executable/non-DDS data, including huge files.
    const auto mods = root / L"dds-only-mods";
    std::filesystem::create_directories(mods / L"arbitrary_mod");
    WriteBytes(mods / L"arbitrary_mod" / L"plugin.dll", nul);
    ModIndexOptions options;
    options.dds_only = true;
    const auto built = BuildModIndex(mods, options);
    Require(built && built.snapshot->Assets().empty() && built.snapshot->ModIds().size() == 1,
            "DDS-only index loaded executable data");
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    try {
        TemporaryDirectory temporary;
        TestVirtualPaths();
        TestHashAndBuildFailClosed(temporary.Path());
        TestDdsValidation();
        TestReadableMemoryRange();
        TestSignatureScanner();
        TestResolverProbeFilter();
        TestResourceIdentityCorrelation();
        TestPackageIdentityCatalog(temporary.Path());
        TestPackageDdsContractCatalog(temporary.Path());
        TestLogger(temporary.Path());
        TestLoaderConfig(temporary.Path());
        TestModIndex(temporary.Path());
        TestGeneralDdsCandidates(temporary.Path());
        TestByteStorageContainment(temporary.Path());
        TestModIndexLimits(temporary.Path());
        TestCanonicalModIdCollision(temporary.Path());
        if (argc >= 2) {
            TestEditedAsset(argv[1]);
        }
        if (argc >= 3) {
            TestSupportedGame(argv[2]);
        }
        if (argc >= 4) {
            TestDeployedMod(argv[3]);
        }
        std::cout << "offline_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "offline_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
