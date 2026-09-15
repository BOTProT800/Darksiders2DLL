#include "package_dds_contract.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

namespace ds2::modding {
namespace {

constexpr std::size_t kCompressedChunkSize = 256 * 1024;
constexpr std::size_t kUncompressedChunkSize = 256 * 1024;

template <typename Unsigned>
[[nodiscard]] bool CheckedAdd(
    const Unsigned left,
    const Unsigned right,
    Unsigned& sum) noexcept {
    static_assert((std::numeric_limits<Unsigned>::is_integer));
    static_assert(!(std::numeric_limits<Unsigned>::is_signed));
    if (right > (std::numeric_limits<Unsigned>::max)() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

[[nodiscard]] bool IdentityLess(
    const PackageResourceIdentity& left,
    const PackageResourceIdentity& right) noexcept {
    if (left.package_base != right.package_base) {
        return left.package_base < right.package_base;
    }
    if (left.member_table_offset != right.member_table_offset) {
        return left.member_table_offset < right.member_table_offset;
    }
    return left.member_ordinal < right.member_ordinal;
}

class PackageFile final {
public:
    PackageFile() noexcept = default;
    ~PackageFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    PackageFile(const PackageFile&) = delete;
    PackageFile& operator=(const PackageFile&) = delete;

    [[nodiscard]] bool Open(
        const std::filesystem::path& path,
        DWORD& error,
        std::wstring& detail) {
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            detail = L"cannot open media.upak for DDS contract extraction";
            return false;
        }

        SetLastError(ERROR_SUCCESS);
        const DWORD file_type = GetFileType(handle_);
        if (file_type != FILE_TYPE_DISK) {
            error = file_type == FILE_TYPE_UNKNOWN
                ? GetLastError() : ERROR_INVALID_DATA;
            if (error == ERROR_SUCCESS) {
                error = ERROR_INVALID_DATA;
            }
            detail = L"media.upak is not a disk file";
            return false;
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                handle_, FileAttributeTagInfo,
                &attributes, sizeof(attributes))) {
            error = GetLastError();
            detail = L"cannot inspect media.upak attributes";
            return false;
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            error = ERROR_REPARSE_TAG_INVALID;
            detail = L"media.upak is a reparse point or directory";
            return false;
        }

        LARGE_INTEGER signed_size{};
        if (!GetFileSizeEx(handle_, &signed_size) || signed_size.QuadPart < 0) {
            error = GetLastError();
            if (error == ERROR_SUCCESS) {
                error = ERROR_INVALID_DATA;
            }
            detail = L"cannot determine media.upak size";
            return false;
        }
        size_ = static_cast<std::uint64_t>(signed_size.QuadPart);
        error = ERROR_SUCCESS;
        return true;
    }

    [[nodiscard]] bool ReadAt(
        const std::uint64_t offset,
        const std::span<std::byte> destination,
        DWORD& error) noexcept {
        std::uint64_t end = 0;
        if (!CheckedAdd(
                offset,
                static_cast<std::uint64_t>(destination.size()), end) ||
            end > size_ ||
            offset > static_cast<std::uint64_t>(
                (std::numeric_limits<LONGLONG>::max)())) {
            error = ERROR_HANDLE_EOF;
            return false;
        }

        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
            error = GetLastError();
            return false;
        }

        std::size_t copied = 0;
        while (copied < destination.size()) {
            const std::size_t remaining = destination.size() - copied;
            const DWORD chunk = static_cast<DWORD>((std::min)(
                remaining,
                static_cast<std::size_t>(
                    (std::numeric_limits<DWORD>::max)())));
            DWORD bytes_read = 0;
            if (!ReadFile(
                    handle_, destination.data() + copied,
                    chunk, &bytes_read, nullptr)) {
                error = GetLastError();
                return false;
            }
            if (bytes_read == 0 || bytes_read > remaining) {
                error = ERROR_HANDLE_EOF;
                return false;
            }
            copied += static_cast<std::size_t>(bytes_read);
        }
        error = ERROR_SUCCESS;
        return true;
    }

    [[nodiscard]] std::uint64_t Size() const noexcept { return size_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::uint64_t size_{};
};

struct StreamGroupKey final {
    std::uint64_t package_base{};
    std::uint32_t payload_offset{};
    std::uint64_t segment_size{};

    [[nodiscard]] bool operator<(const StreamGroupKey& other) const noexcept {
        if (package_base != other.package_base) {
            return package_base < other.package_base;
        }
        if (payload_offset != other.payload_offset) {
            return payload_offset < other.payload_offset;
        }
        return segment_size < other.segment_size;
    }
};

struct PendingTarget final {
    const PackageIdentityEntry* source{};
    std::vector<std::byte> bytes;
    std::uint64_t copied{};
};

class InflateGuard final {
public:
    explicit InflateGuard(z_stream& stream) noexcept : stream_(stream) {}
    ~InflateGuard() { static_cast<void>(inflateEnd(&stream_)); }
    InflateGuard(const InflateGuard&) = delete;
    InflateGuard& operator=(const InflateGuard&) = delete;

private:
    z_stream& stream_;
};

void AddIssue(
    PackageDdsContractCatalogBuildResult& result,
    const PackageDdsContractCatalogOptions& options,
    const PackageIdentityEntry& entry,
    const PackageDdsContractCatalogIssueCode code,
    std::wstring detail) {
    if (result.issues.size() >= options.max_issue_count) {
        if (result.dropped_issue_count !=
            (std::numeric_limits<std::size_t>::max)()) {
            ++result.dropped_issue_count;
        }
        return;
    }
    result.issues.push_back(PackageDdsContractCatalogIssue{
        code, entry.identity, entry.virtual_path.display, std::move(detail)});
}

[[nodiscard]] bool Fail(
    PackageDdsContractCatalogBuildResult& result,
    const PackageDdsContractCatalogError error,
    const DWORD system_error,
    const int zlib_error,
    std::wstring detail) {
    result.snapshot.reset();
    result.error = error;
    result.system_error = system_error;
    result.zlib_error = zlib_error;
    result.detail = std::move(detail);
    return false;
}

[[nodiscard]] std::uint32_t ReadU32(
    const std::array<std::byte, sizeof(std::uint32_t)>& bytes) noexcept {
    return std::to_integer<std::uint32_t>(bytes[0]) |
        (std::to_integer<std::uint32_t>(bytes[1]) << 8) |
        (std::to_integer<std::uint32_t>(bytes[2]) << 16) |
        (std::to_integer<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] bool InflateGroup(
    PackageFile& package,
    const StreamGroupKey& key,
    const std::span<const PackageIdentityEntry* const> source_entries,
    const PackageDdsContractCatalogOptions& options,
    std::vector<PackageDdsContractEntry>& entries,
    PackageDdsContractCatalogBuildResult& result) {
    std::uint64_t segment_end = 0;
    std::uint64_t size_position = 0;
    std::uint64_t compressed_begin = 0;
    if (!CheckedAdd(key.package_base, key.segment_size, segment_end) ||
        segment_end > package.Size() ||
        !CheckedAdd(
            key.package_base,
            static_cast<std::uint64_t>(key.payload_offset), size_position) ||
        !CheckedAdd(
            size_position,
            static_cast<std::uint64_t>(sizeof(std::uint32_t)),
            compressed_begin) ||
        compressed_begin >= segment_end) {
        return Fail(
            result, PackageDdsContractCatalogError::invalid_package,
            ERROR_INVALID_DATA, Z_OK,
            L"a DDS contract stream is outside its package segment");
    }

    const std::uint64_t compressed_size = segment_end - compressed_begin;
    if (compressed_size > options.max_compressed_stream_bytes) {
        for (const auto* const source : source_entries) {
            AddIssue(
                result, options, *source,
                PackageDdsContractCatalogIssueCode::stream_too_large,
                L"compressed OBPK stream exceeds its diagnostic limit");
        }
        return true;
    }

    std::array<std::byte, sizeof(std::uint32_t)> declared_bytes{};
    DWORD read_error = ERROR_SUCCESS;
    if (!package.ReadAt(size_position, declared_bytes, read_error)) {
        return Fail(
            result, PackageDdsContractCatalogError::read_failed,
            read_error, Z_OK,
            L"cannot read the OBPK stream's declared output size");
    }
    const std::uint64_t declared_size = ReadU32(declared_bytes);
    if (declared_size == 0) {
        return Fail(
            result, PackageDdsContractCatalogError::invalid_package,
            ERROR_INVALID_DATA, Z_OK,
            L"a requested OBPK stream declares an empty output");
    }
    if (declared_size > options.max_uncompressed_stream_bytes) {
        for (const auto* const source : source_entries) {
            AddIssue(
                result, options, *source,
                PackageDdsContractCatalogIssueCode::stream_too_large,
                L"uncompressed OBPK stream exceeds its diagnostic limit");
        }
        return true;
    }

    std::vector<PendingTarget> targets;
    targets.reserve(source_entries.size());
    for (const auto* const source : source_entries) {
        std::uint64_t member_end = 0;
        if (!CheckedAdd(
                source->uncompressed_offset,
                static_cast<std::uint64_t>(source->original_size),
                member_end) ||
            member_end > declared_size) {
            return Fail(
                result, PackageDdsContractCatalogError::invalid_package,
                ERROR_INVALID_DATA, Z_OK,
                L"a requested DDS member lies outside its OBPK stream");
        }
        targets.push_back(PendingTarget{
            source,
            std::vector<std::byte>(source->original_size),
            0});
    }
    std::sort(
        targets.begin(), targets.end(),
        [](const PendingTarget& left, const PendingTarget& right) noexcept {
            return left.source->uncompressed_offset <
                right.source->uncompressed_offset;
        });

    z_stream stream{};
    const int initialize_status = inflateInit(&stream);
    if (initialize_status != Z_OK) {
        return Fail(
            result,
            initialize_status == Z_MEM_ERROR
                ? PackageDdsContractCatalogError::allocation_failed
                : PackageDdsContractCatalogError::decompression_failed,
            ERROR_SUCCESS, initialize_status,
            L"zlib could not initialize the OBPK stream decoder");
    }
    InflateGuard guard(stream);

    std::array<std::byte, kCompressedChunkSize> compressed{};
    std::array<std::byte, kUncompressedChunkSize> output{};
    std::uint64_t compressed_cursor = compressed_begin;
    std::uint64_t produced_total = 0;
    std::size_t first_possible_target = 0;
    int inflate_status = Z_OK;

    while (inflate_status != Z_STREAM_END) {
        if (stream.avail_in == 0) {
            if (compressed_cursor >= segment_end) {
                return Fail(
                    result, PackageDdsContractCatalogError::decompression_failed,
                    ERROR_HANDLE_EOF, Z_BUF_ERROR,
                    L"OBPK zlib stream ended before a valid zlib EOF");
            }
            const auto input_size = static_cast<std::size_t>((std::min)(
                static_cast<std::uint64_t>(compressed.size()),
                segment_end - compressed_cursor));
            if (!package.ReadAt(
                    compressed_cursor,
                    std::span(compressed.data(), input_size),
                    read_error)) {
                return Fail(
                    result, PackageDdsContractCatalogError::read_failed,
                    read_error, Z_OK,
                    L"cannot read compressed OBPK stream bytes");
            }
            compressed_cursor += input_size;
            stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
            stream.avail_in = static_cast<uInt>(input_size);
        }

        stream.next_out = reinterpret_cast<Bytef*>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        const uInt input_before = stream.avail_in;
        inflate_status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = static_cast<std::size_t>(
            output.size() - stream.avail_out);
        const auto consumed = static_cast<std::size_t>(
            input_before - stream.avail_in);

        std::uint64_t next_total = 0;
        if (!CheckedAdd(
                produced_total,
                static_cast<std::uint64_t>(produced), next_total) ||
            next_total > declared_size) {
            return Fail(
                result, PackageDdsContractCatalogError::invalid_package,
                ERROR_INVALID_DATA, inflate_status,
                L"OBPK zlib output exceeds its declared size");
        }

        const std::uint64_t chunk_begin = produced_total;
        const std::uint64_t chunk_end = next_total;
        while (first_possible_target < targets.size()) {
            std::uint64_t target_end = 0;
            static_cast<void>(CheckedAdd(
                targets[first_possible_target].source->uncompressed_offset,
                static_cast<std::uint64_t>(
                    targets[first_possible_target].source->original_size),
                target_end));
            if (target_end > chunk_begin) {
                break;
            }
            ++first_possible_target;
        }
        for (std::size_t index = first_possible_target;
             index < targets.size(); ++index) {
            PendingTarget& target = targets[index];
            const std::uint64_t target_begin =
                target.source->uncompressed_offset;
            if (target_begin >= chunk_end) {
                break;
            }
            const std::uint64_t target_end = target_begin +
                static_cast<std::uint64_t>(target.source->original_size);
            const std::uint64_t overlap_begin = (std::max)(
                chunk_begin, target_begin);
            const std::uint64_t overlap_end = (std::min)(
                chunk_end, target_end);
            if (overlap_begin < overlap_end) {
                const auto count = static_cast<std::size_t>(
                    overlap_end - overlap_begin);
                const auto source_offset = static_cast<std::size_t>(
                    overlap_begin - chunk_begin);
                const auto target_offset = static_cast<std::size_t>(
                    overlap_begin - target_begin);
                std::memcpy(
                    target.bytes.data() + target_offset,
                    output.data() + source_offset,
                    count);
                target.copied += count;
            }
        }
        produced_total = next_total;

        if (inflate_status != Z_OK && inflate_status != Z_STREAM_END) {
            return Fail(
                result, PackageDdsContractCatalogError::decompression_failed,
                ERROR_INVALID_DATA, inflate_status,
                L"zlib rejected the OBPK stream");
        }
        if (inflate_status == Z_OK && produced == 0 && consumed == 0) {
            return Fail(
                result, PackageDdsContractCatalogError::decompression_failed,
                ERROR_INVALID_DATA, Z_BUF_ERROR,
                L"OBPK zlib decoder made no progress");
        }
    }

    if (produced_total != declared_size) {
        return Fail(
            result, PackageDdsContractCatalogError::invalid_package,
            ERROR_INVALID_DATA, Z_STREAM_END,
            L"OBPK zlib output does not equal its declared size");
    }

    for (auto& target : targets) {
        if (target.copied != target.bytes.size()) {
            return Fail(
                result, PackageDdsContractCatalogError::invalid_package,
                ERROR_INVALID_DATA, Z_STREAM_END,
                L"a requested DDS interval was not fully recovered");
        }
        auto validated = ValidateDds(target.bytes);
        if (!validated) {
            AddIssue(
                result, options, *target.source,
                PackageDdsContractCatalogIssueCode::invalid_original_dds,
                std::wstring(DdsValidationErrorName(validated.error)) +
                    L": " + validated.detail);
            continue;
        }

        const Sha256Result full_hash = ComputeSha256(target.bytes);
        const DdsMetadata& dds = *validated.metadata;
        const auto payload_offset = static_cast<std::size_t>(dds.header_size);
        const Sha256Result payload_hash = ComputeSha256(
            std::span(target.bytes).subspan(payload_offset));
        if (!full_hash || !payload_hash) {
            return Fail(
                result, PackageDdsContractCatalogError::hash_failed,
                full_hash ? payload_hash.win32_error : full_hash.win32_error,
                Z_OK,
                L"cannot hash a recovered original DDS contract");
        }

        entries.push_back(PackageDdsContractEntry{
            target.source->identity,
            target.source->virtual_path,
            dds,
            full_hash.digest,
            payload_hash.digest});
    }
    return true;
}

}  // namespace

PackageDdsContractCatalogSnapshot::PackageDdsContractCatalogSnapshot(
    std::vector<PackageDdsContractEntry> entries) noexcept
    : entries_(std::move(entries)) {}

const PackageDdsContractEntry* PackageDdsContractCatalogSnapshot::Find(
    const PackageResourceIdentity& identity) const noexcept {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), identity,
        [](const PackageDdsContractEntry& entry,
           const PackageResourceIdentity& key) noexcept {
            return IdentityLess(entry.identity, key);
        });
    if (found == entries_.end() || !(found->identity == identity)) {
        return nullptr;
    }
    return std::addressof(*found);
}

PackageDdsContractCatalogBuildResult BuildMediaPackageDdsContractCatalog(
    const std::filesystem::path& game_directory,
    const PackageIdentityCatalogSnapshot* const package_catalog,
    const PackageDdsContractCatalogOptions& options) {
    PackageDdsContractCatalogBuildResult result;
    if (game_directory.empty() || package_catalog == nullptr ||
        options.max_asset_bytes == 0 ||
        options.max_total_asset_bytes == 0 ||
        options.max_compressed_stream_bytes == 0 ||
        options.max_uncompressed_stream_bytes == 0 ||
        options.max_issue_count == 0) {
        static_cast<void>(Fail(
            result, PackageDdsContractCatalogError::invalid_argument,
            ERROR_INVALID_PARAMETER, Z_OK,
            L"invalid DDS contract catalog input or limits"));
        return result;
    }

    try {
        PackageFile package;
        DWORD open_error = ERROR_SUCCESS;
        std::wstring open_detail;
        if (!package.Open(
                game_directory / L"media" / L"media.upak",
                open_error, open_detail)) {
            result.error = PackageDdsContractCatalogError::package_open_failed;
            result.system_error = open_error;
            result.detail = std::move(open_detail);
            return result;
        }

        std::uint64_t selected_bytes = 0;
        std::map<StreamGroupKey, std::vector<const PackageIdentityEntry*>> groups;
        for (const auto& entry : package_catalog->Entries()) {
            if (entry.type_id != 6) {
                AddIssue(
                    result, options, entry,
                    PackageDdsContractCatalogIssueCode::unsupported_type,
                    L"package member is not a DDS resource");
                continue;
            }
            if (entry.original_size == 0 ||
                entry.original_size > options.max_asset_bytes) {
                AddIssue(
                    result, options, entry,
                    PackageDdsContractCatalogIssueCode::asset_too_large,
                    L"original DDS size is empty or exceeds its diagnostic limit");
                continue;
            }
            std::uint64_t next_selected = 0;
            if (!CheckedAdd(
                    selected_bytes,
                    static_cast<std::uint64_t>(entry.original_size),
                    next_selected) ||
                next_selected > options.max_total_asset_bytes) {
                AddIssue(
                    result, options, entry,
                    PackageDdsContractCatalogIssueCode::total_budget_exceeded,
                    L"original DDS aggregate byte budget is exhausted");
                continue;
            }
            selected_bytes = next_selected;
            groups[StreamGroupKey{
                entry.identity.package_base,
                entry.identity.member_table_offset,
                entry.segment_size}].push_back(std::addressof(entry));
        }

        std::vector<PackageDdsContractEntry> entries;
        entries.reserve(package_catalog->Entries().size());
        for (const auto& [key, sources] : groups) {
            if (!InflateGroup(
                    package, key, sources, options, entries, result)) {
                return result;
            }
        }

        std::sort(
            entries.begin(), entries.end(),
            [](const PackageDdsContractEntry& left,
               const PackageDdsContractEntry& right) noexcept {
                return IdentityLess(left.identity, right.identity);
            });
        for (std::size_t index = 1; index < entries.size(); ++index) {
            if (entries[index - 1].identity == entries[index].identity) {
                static_cast<void>(Fail(
                    result, PackageDdsContractCatalogError::invalid_package,
                    ERROR_DUP_NAME, Z_OK,
                    L"multiple original DDS contracts share one package identity"));
                return result;
            }
        }

        result.snapshot = std::shared_ptr<const PackageDdsContractCatalogSnapshot>(
            new PackageDdsContractCatalogSnapshot(std::move(entries)));
        return result;
    } catch (const std::bad_alloc&) {
        static_cast<void>(Fail(
            result, PackageDdsContractCatalogError::allocation_failed,
            ERROR_NOT_ENOUGH_MEMORY, Z_MEM_ERROR,
            L"DDS contract catalog allocation failed"));
        return result;
    } catch (const std::length_error&) {
        static_cast<void>(Fail(
            result, PackageDdsContractCatalogError::allocation_failed,
            ERROR_NOT_ENOUGH_MEMORY, Z_MEM_ERROR,
            L"DDS contract catalog length is not representable"));
        return result;
    } catch (...) {
        static_cast<void>(Fail(
            result, PackageDdsContractCatalogError::invalid_package,
            ERROR_INVALID_DATA, Z_DATA_ERROR,
            L"unexpected DDS contract catalog exception"));
        return result;
    }
}

std::wstring_view PackageDdsContractCatalogErrorName(
    const PackageDdsContractCatalogError error) noexcept {
    switch (error) {
    case PackageDdsContractCatalogError::none: return L"none";
    case PackageDdsContractCatalogError::invalid_argument: return L"invalid_argument";
    case PackageDdsContractCatalogError::package_open_failed: return L"package_open_failed";
    case PackageDdsContractCatalogError::invalid_package: return L"invalid_package";
    case PackageDdsContractCatalogError::read_failed: return L"read_failed";
    case PackageDdsContractCatalogError::decompression_failed: return L"decompression_failed";
    case PackageDdsContractCatalogError::hash_failed: return L"hash_failed";
    case PackageDdsContractCatalogError::allocation_failed: return L"allocation_failed";
    }
    return L"unknown";
}

std::wstring_view PackageDdsContractCatalogIssueCodeName(
    const PackageDdsContractCatalogIssueCode code) noexcept {
    switch (code) {
    case PackageDdsContractCatalogIssueCode::unsupported_type:
        return L"unsupported_type";
    case PackageDdsContractCatalogIssueCode::asset_too_large:
        return L"asset_too_large";
    case PackageDdsContractCatalogIssueCode::total_budget_exceeded:
        return L"total_budget_exceeded";
    case PackageDdsContractCatalogIssueCode::stream_too_large:
        return L"stream_too_large";
    case PackageDdsContractCatalogIssueCode::invalid_original_dds:
        return L"invalid_original_dds";
    }
    return L"unknown";
}

}  // namespace ds2::modding
