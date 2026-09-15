#include "pch.h"

#include "bootstrap.h"

#include "loader_features.h"
#include "loader_config.h"
#include "game_build.h"
#include "logging.h"
#include "mod_index.h"
#if defined(DS2_GENERAL_DDS_ENABLED)
#include "general_dds_candidate.h"
#include "package_dds_contract.h"
#include "package_identity_catalog.h"
#endif
#include "resolver_probe.h"
#include "sha256.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#if defined(DS2_GENERAL_DDS_ENABLED)
#include <vector>
#endif

#include <ShlObj.h>

namespace
{
    INIT_ONCE g_bootstrapInitOnce = INIT_ONCE_STATIC_INIT;
    std::atomic<ds2::bootstrap::Status> g_bootstrapStatus =
        ds2::bootstrap::Status::not_started;
    std::atomic<std::shared_ptr<ds2::modding::SessionLogger>> g_logger;
    std::atomic<std::shared_ptr<const ds2::modding::ModIndexSnapshot>> g_mod_index;
#if defined(DS2_GENERAL_DDS_ENABLED)
    std::atomic<std::shared_ptr<
        const ds2::modding::PackageIdentityCatalogSnapshot>>
        g_package_identity_catalog;
    std::atomic<std::shared_ptr<
        const ds2::modding::GeneralDdsCandidateSnapshot>>
        g_general_dds_candidates;
#endif
    std::atomic<unsigned long> g_pending_resolver_events{};
    std::atomic<HANDLE> g_resolver_event_handle{};
    std::atomic<std::uintptr_t> g_game_image_base{};

    constexpr unsigned long kResolverFirstEntryEvent = 1u << 0;
    constexpr unsigned long kResolverQueueEvent = 1u << 1;
    constexpr DWORD kResolverStatsIntervalMilliseconds = 5'000;

    [[nodiscard]] std::optional<std::filesystem::path> GetLogDirectory()
    {
        PWSTR raw_path = nullptr;
        const HRESULT result = SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw_path);
        if (FAILED(result) || raw_path == nullptr)
        {
            return std::nullopt;
        }

        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(
            raw_path, CoTaskMemFree);
        std::filesystem::path path(owned_path.get());
        return path / L"Darksiders2DLL" / L"logs";
    }

    void LogEvent(
        const std::wstring_view event_name,
        const std::wstring_view detail = {}) noexcept
    {
        const auto logger = g_logger.load(std::memory_order_acquire);
        if (logger != nullptr)
        {
            static_cast<void>(logger->Event(event_name, detail));
        }
    }

    [[nodiscard]] const wchar_t* BoolName(const bool value) noexcept
    {
        return value ? L"true" : L"false";
    }

    void AppendHex(std::wstring& output, const std::uint64_t value)
    {
        std::array<wchar_t, 32> buffer{};
        const int length = swprintf_s(
            buffer.data(), buffer.size(), L"0x%llX",
            static_cast<unsigned long long>(value));
        if (length > 0)
        {
            output.append(buffer.data(), static_cast<std::size_t>(length));
        }
    }

    void AppendResolverStack(
        std::wstring& output,
        const ds2::modding::ResolverProbeEvent& event)
    {
        output.append(L" stack=");
        const std::uintptr_t image_base =
            g_game_image_base.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < event.frame_count; ++index)
        {
            if (index != 0)
            {
                output.push_back(L',');
            }
            const std::uintptr_t address = event.frames[index];
            if (image_base != 0 && address >= image_base &&
                address - image_base < 0x04000000ull)
            {
                output.append(L"game+");
                AppendHex(output, address - image_base);
            }
            else
            {
                AppendHex(output, address);
            }
        }
    }

    void LogResolverProbeEvent(
        const ds2::modding::ResolverProbeEvent& event)
    {
        std::wstring detail = L"qpc=";
        detail.append(std::to_wstring(event.performance_counter));
        detail.append(L" tid=");
        detail.append(std::to_wstring(event.thread_id));

#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
        if (event.kind ==
            ds2::modding::ResolverProbeEventKind::resource_identity_sample)
        {
            const auto& sample = event.resource_identity;
            detail.append(L" sequence=");
            detail.append(std::to_wstring(sample.sequence));
            detail.append(L" depth=");
            detail.append(std::to_wstring(sample.nesting_depth));
            detail.append(L" read_ordinal=");
            detail.append(std::to_wstring(sample.read_ordinal));
            detail.append(L" owner=");
            AppendHex(detail, sample.scope.owner);
            detail.append(L" arg2=");
            AppendHex(detail, sample.scope.argument2);
            detail.append(L" arg3=");
            AppendHex(detail, sample.scope.argument3);
            detail.append(L" arg4=");
            AppendHex(detail, sample.scope.argument4);
            detail.append(L" arg5=");
            AppendHex(detail, sample.scope.argument5);
            detail.append(L" scope_stream=");
            AppendHex(detail, sample.scope.stream);
            detail.append(L" stream_valid=");
            detail.append(BoolName(sample.scope.stream_valid));
            detail.append(L" object_fields_valid=");
            detail.append(BoolName(sample.scope.object_fields_valid));
            detail.append(L" package_base=");
            AppendHex(detail, sample.scope.package_base);
            detail.append(L" member_table_offset=");
            detail.append(std::to_wstring(sample.scope.member_table_offset));
            detail.append(L" outer_caller_rva=");
            AppendHex(detail, sample.scope.caller_rva);
            detail.append(L" read_stream=");
            AppendHex(detail, sample.read.stream);
            detail.append(L" destination=");
            AppendHex(detail, sample.read.destination);
            detail.append(L" read_caller_rva=");
            AppendHex(detail, sample.read.caller_rva);
            detail.append(L" requested=");
            detail.append(std::to_wstring(sample.read.requested));
            detail.append(L" returned=");
            detail.append(std::to_wstring(sample.read.returned));
            if (sample.read.hash_valid)
            {
                detail.append(L" sha256=");
                detail.append(ds2::modding::Sha256HexWide(sample.read.sha256));
            }
            else
            {
                detail.append(L" sha256=unavailable");
            }
#if defined(DS2_GENERAL_DDS_ENABLED)
            const auto catalog =
                g_package_identity_catalog.load(std::memory_order_acquire);
            if (catalog != nullptr && sample.scope.object_fields_valid &&
                sample.scope.stream_valid &&
                sample.scope.stream != 0 && sample.read.stream != 0 &&
                sample.scope.member_table_offset >= 0)
            {
                const ds2::modding::PackageResourceIdentity identity{
                    sample.scope.package_base,
                    static_cast<std::uint32_t>(
                        sample.scope.member_table_offset),
                    sample.read_ordinal};
                const auto* const mapped = catalog->Find(identity);
                if (mapped != nullptr)
                {
                    detail.append(L" resolved_path=");
                    detail.append(mapped->virtual_path.key);
                }
                else
                {
                    detail.append(L" resolved_path=unmapped");
                }
            }
#endif
            AppendResolverStack(detail, event);
            LogEvent(L"RESOURCE_IDENTITY_SAMPLE", detail);
            return;
        }
#endif

#if defined(DS2_GENERAL_DDS_ENABLED)
        if (event.kind ==
            ds2::modding::ResolverProbeEventKind::general_dds_dry_run)
        {
            const auto& dry_run = event.general_dds;
            detail.append(L" decision=");
            detail.append(ds2::modding::GeneralDdsDryRunDecisionName(
                dry_run.decision));
            detail.append(L" sequence=");
            detail.append(std::to_wstring(
                event.resource_identity.sequence));
            detail.append(L" read_ordinal=");
            detail.append(std::to_wstring(
                dry_run.identity.member_ordinal));
            detail.append(L" package_base=");
            AppendHex(detail, dry_run.identity.package_base);
            detail.append(L" member_table_offset=");
            AppendHex(detail, dry_run.identity.member_table_offset);
            detail.append(L" scope_stream=");
            AppendHex(detail, event.resource_identity.scope.stream);
            detail.append(L" read_stream=");
            AppendHex(detail, event.resource_identity.read.stream);
            detail.append(L" destination=");
            AppendHex(detail, event.destination);
            detail.append(L" destination_valid=");
            detail.append(BoolName(dry_run.destination_valid));
            detail.append(L" source_hash_valid=");
            detail.append(BoolName(dry_run.source_hash_valid));
            if (dry_run.source_hash_valid)
            {
                detail.append(L" source_sha256=");
                detail.append(ds2::modding::Sha256HexWide(
                    dry_run.source_sha256));
            }
            detail.append(L" requested=");
            detail.append(std::to_wstring(event.requested));
            detail.append(L" returned=");
            detail.append(std::to_wstring(event.returned));
            detail.append(L" original_size=");
            detail.append(std::to_wstring(dry_run.original_size));
            detail.append(L" payload_size=");
            detail.append(std::to_wstring(dry_run.payload_size));
            detail.append(L" win32=");
            detail.append(std::to_wstring(event.win32_error));
            detail.append(L" buffer_writes=");
            detail.append(BoolName(dry_run.replacement_written));
            detail.append(L" replacement_written=");
            detail.append(BoolName(dry_run.replacement_written));
            detail.append(L" replacement_verified=");
            detail.append(BoolName(dry_run.replacement_verified));
            detail.append(L" replacement_size=");
            detail.append(std::to_wstring(dry_run.replacement_size));
            detail.append(L" replacement_win32=");
            detail.append(std::to_wstring(
                dry_run.replacement_win32_error));

            const auto candidates =
                g_general_dds_candidates.load(std::memory_order_acquire);
            const auto* const candidate = candidates == nullptr
                ? nullptr
                : candidates->Find(dry_run.identity);
            if (candidate != nullptr)
            {
                detail.append(L" mod=");
                detail.append(candidate->mod_id);
                detail.append(L" path=");
                detail.append(candidate->virtual_path.key);
            }

            AppendResolverStack(detail, event);
            const bool would_override =
                dry_run.decision ==
                    ds2::modding::GeneralDdsDryRunDecision::full_dds ||
                dry_run.decision ==
                    ds2::modding::GeneralDdsDryRunDecision::payload;
            if (!would_override)
            {
                LogEvent(L"GENERAL_DDS_CONTRACT_REJECTED", detail);
            }
#if defined(DS2_GENERAL_DDS_WRITE_ENABLED)
            else if (dry_run.replacement_written &&
                     dry_run.replacement_verified)
            {
                LogEvent(L"GENERAL_DDS_OVERRIDE_HIT", detail);
            }
            else if (!dry_run.replacement_attempted)
            {
                LogEvent(L"GENERAL_DDS_VERIFIED_WOULD_OVERRIDE", detail);
            }
            else
            {
                LogEvent(L"GENERAL_DDS_OVERRIDE_FAILED", detail);
            }
#else
            else
            {
                LogEvent(L"GENERAL_DDS_VERIFIED_WOULD_OVERRIDE", detail);
            }
#endif
            return;
        }
#endif

        if (event.kind == ds2::modding::ResolverProbeEventKind::package_segment_read)
        {
            detail.append(L" offset=");
            AppendHex(detail, event.file_offset);
            detail.append(L" requested=");
            detail.append(std::to_wstring(event.requested));
            detail.append(L" returned=");
            detail.append(std::to_wstring(event.returned));
            detail.append(L" success=");
            detail.append(BoolName(event.call_succeeded));
            detail.append(L" win32=");
            detail.append(std::to_wstring(event.win32_error));
            detail.append(L" buffer=");
            AppendHex(detail, event.destination);
            AppendResolverStack(detail, event);
            LogEvent(L"PACKAGE_SEGMENT_READ", detail);
            return;
        }

    }

    [[nodiscard]] bool ResolverStatsEqual(
        const ds2::modding::ResolverProbeStats& left,
        const ds2::modding::ResolverProbeStats& right) noexcept
    {
        return left.read_file_calls == right.read_file_calls &&
            left.package_segment_reads == right.package_segment_reads &&
            left.target_sized_stream_reads == right.target_sized_stream_reads &&
            left.queued_events == right.queued_events &&
            left.dropped_events == right.dropped_events &&
            left.sample_copy_failures == right.sample_copy_failures &&
            left.target_hash_matches == right.target_hash_matches &&
            left.replacements_applied == right.replacements_applied &&
            left.replacement_write_failures == right.replacement_write_failures
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
            &&
            left.resource_identity_scopes == right.resource_identity_scopes &&
            left.resource_identity_samples == right.resource_identity_samples &&
            left.resource_identity_overflows == right.resource_identity_overflows &&
            left.resource_identity_context_resets ==
                right.resource_identity_context_resets
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
            &&
            left.general_candidate_hits == right.general_candidate_hits &&
            left.general_would_override == right.general_would_override &&
            left.general_contract_mismatches ==
                right.general_contract_mismatches &&
            left.general_source_hash_failures ==
                right.general_source_hash_failures &&
            left.general_source_hash_mismatches ==
                right.general_source_hash_mismatches &&
            left.general_write_attempts == right.general_write_attempts &&
            left.general_writes_completed == right.general_writes_completed &&
            left.general_write_failures == right.general_write_failures &&
            left.general_write_verify_failures ==
                right.general_write_verify_failures
#endif
            ;
    }

    void LogResolverStats(const ds2::modding::ResolverProbeStats& stats)
    {
        std::wstring detail = L"read_file_calls=";
        detail.append(std::to_wstring(stats.read_file_calls));
        detail.append(L" package_segment_reads=");
        detail.append(std::to_wstring(stats.package_segment_reads));
        detail.append(L" target_sized_stream_reads=");
        detail.append(std::to_wstring(stats.target_sized_stream_reads));
        detail.append(L" queued_events=");
        detail.append(std::to_wstring(stats.queued_events));
        detail.append(L" dropped_events=");
        detail.append(std::to_wstring(stats.dropped_events));
        detail.append(L" sample_copy_failures=");
        detail.append(std::to_wstring(stats.sample_copy_failures));
        detail.append(L" target_hash_matches=");
        detail.append(std::to_wstring(stats.target_hash_matches));
        detail.append(L" replacements_applied=");
        detail.append(std::to_wstring(stats.replacements_applied));
        detail.append(L" replacement_write_failures=");
        detail.append(std::to_wstring(stats.replacement_write_failures));
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
        detail.append(L" resource_identity_scopes=");
        detail.append(std::to_wstring(stats.resource_identity_scopes));
        detail.append(L" resource_identity_samples=");
        detail.append(std::to_wstring(stats.resource_identity_samples));
        detail.append(L" resource_identity_overflows=");
        detail.append(std::to_wstring(stats.resource_identity_overflows));
        detail.append(L" resource_identity_context_resets=");
        detail.append(std::to_wstring(stats.resource_identity_context_resets));
#endif
#if defined(DS2_GENERAL_DDS_ENABLED)
        detail.append(L" general_candidate_hits=");
        detail.append(std::to_wstring(stats.general_candidate_hits));
        detail.append(L" general_would_override=");
        detail.append(std::to_wstring(stats.general_would_override));
        detail.append(L" general_contract_mismatches=");
        detail.append(std::to_wstring(stats.general_contract_mismatches));
        detail.append(L" general_source_hash_failures=");
        detail.append(std::to_wstring(stats.general_source_hash_failures));
        detail.append(L" general_source_hash_mismatches=");
        detail.append(std::to_wstring(stats.general_source_hash_mismatches));
        detail.append(L" general_write_attempts=");
        detail.append(std::to_wstring(stats.general_write_attempts));
        detail.append(L" general_writes_completed=");
        detail.append(std::to_wstring(stats.general_writes_completed));
        detail.append(L" general_write_failures=");
        detail.append(std::to_wstring(stats.general_write_failures));
        detail.append(L" general_write_verify_failures=");
        detail.append(std::to_wstring(
            stats.general_write_verify_failures));
#endif
        LogEvent(L"RESOLVER_PROBE_STATS", detail);
    }

#if defined(DS2_GENERAL_DDS_ENABLED)
    void InitializeDdsCatalog(
        const std::filesystem::path& game_directory,
        const ds2::modding::ModIndexSnapshot& mod_index) noexcept
    {
        try
        {
            std::vector<ds2::modding::CanonicalVirtualPath> requested;
            requested.reserve(mod_index.Assets().size());
            for (const auto& asset : mod_index.Assets())
            {
                requested.push_back(asset.virtual_path);
            }

            auto built = ds2::modding::BuildMediaPackageIdentityCatalog(
                game_directory, requested);
            if (!built)
            {
                std::wstring detail = L"reason=";
                detail.append(ds2::modding::PackageIdentityCatalogErrorName(
                    built.error));
                detail.append(L" win32=");
                detail.append(std::to_wstring(built.system_error));
                if (!built.detail.empty())
                {
                    detail.append(L" detail=");
                    detail.append(built.detail);
                }
                LogEvent(L"PACKAGE_IDENTITY_CATALOG_FAILED", detail);
                return;
            }

            for (const auto& issue : built.issues)
            {
                std::wstring detail = L"code=";
                detail.append(
                    ds2::modding::PackageIdentityCatalogIssueCodeName(
                        issue.code));
                detail.append(L" path=");
                detail.append(issue.virtual_path);
                LogEvent(L"PACKAGE_IDENTITY_ISSUE", detail);
            }

            for (const auto& entry : built.snapshot->Entries())
            {
                std::wstring detail = L"package_base=";
                AppendHex(detail, entry.identity.package_base);
                detail.append(L" member_table_offset=");
                AppendHex(detail, entry.identity.member_table_offset);
                detail.append(L" member_ordinal=");
                detail.append(std::to_wstring(
                    entry.identity.member_ordinal));
                detail.append(L" original_size=");
                detail.append(std::to_wstring(entry.original_size));
                detail.append(L" path=");
                detail.append(entry.virtual_path.key);
                LogEvent(L"PACKAGE_IDENTITY_MAPPED", detail);
            }

            std::wstring summary = L"requested=";
            summary.append(std::to_wstring(requested.size()));
            summary.append(L" mapped=");
            summary.append(std::to_wstring(
                built.snapshot->Entries().size()));
            summary.append(L" issues=");
            summary.append(std::to_wstring(built.issues.size()));
            summary.append(L" dropped_issues=");
            summary.append(std::to_wstring(built.dropped_issue_count));
            LogEvent(L"PACKAGE_IDENTITY_CATALOG_READY", summary);

            ds2::modding::PackageDdsContractCatalogOptions contract_options;
            contract_options.max_asset_bytes = 64ull * 1024 * 1024;
            contract_options.max_total_asset_bytes = 256ull * 1024 * 1024;
            auto contracts =
                ds2::modding::BuildMediaPackageDdsContractCatalog(
                    game_directory, built.snapshot.get(), contract_options);
            if (!contracts)
            {
                std::wstring detail = L"reason=";
                detail.append(
                    ds2::modding::PackageDdsContractCatalogErrorName(
                        contracts.error));
                detail.append(L" win32=");
                detail.append(std::to_wstring(contracts.system_error));
                detail.append(L" zlib=");
                detail.append(std::to_wstring(contracts.zlib_error));
                if (!contracts.detail.empty())
                {
                    detail.append(L" detail=");
                    detail.append(contracts.detail);
                }
                LogEvent(L"PACKAGE_DDS_CONTRACT_CATALOG_FAILED", detail);
                g_package_identity_catalog.store(
                    std::move(built.snapshot), std::memory_order_release);
                return;
            }

            for (const auto& issue : contracts.issues)
            {
                std::wstring detail = L"code=";
                detail.append(
                    ds2::modding::PackageDdsContractCatalogIssueCodeName(
                        issue.code));
                detail.append(L" path=");
                detail.append(issue.virtual_path);
                if (!issue.detail.empty())
                {
                    detail.append(L" detail=");
                    detail.append(issue.detail);
                }
                LogEvent(L"PACKAGE_DDS_CONTRACT_ISSUE", detail);
            }

            for (const auto& contract : contracts.snapshot->Entries())
            {
                std::wstring detail = L"path=";
                detail.append(contract.virtual_path.key);
                detail.append(L" package_base=");
                AppendHex(detail, contract.identity.package_base);
                detail.append(L" member_table_offset=");
                AppendHex(detail, contract.identity.member_table_offset);
                detail.append(L" member_ordinal=");
                detail.append(std::to_wstring(
                    contract.identity.member_ordinal));
                detail.append(L" dds=");
                detail.append(std::to_wstring(contract.dds.width));
                detail.push_back(L'x');
                detail.append(std::to_wstring(contract.dds.height));
                detail.push_back(L'/');
                detail.append(ds2::modding::DdsFormatName(
                    contract.dds.format));
                detail.append(L"/mips=");
                detail.append(std::to_wstring(contract.dds.mip_count));
                detail.append(L" full_sha256=");
                detail.append(ds2::modding::Sha256HexWide(
                    contract.full_sha256));
                detail.append(L" payload_sha256=");
                detail.append(ds2::modding::Sha256HexWide(
                    contract.payload_sha256));
                LogEvent(L"PACKAGE_DDS_CONTRACT", detail);
            }

            std::wstring contract_summary = L"contracts=";
            contract_summary.append(std::to_wstring(
                contracts.snapshot->Entries().size()));
            contract_summary.append(L" issues=");
            contract_summary.append(std::to_wstring(contracts.issues.size()));
            contract_summary.append(L" dropped_issues=");
            contract_summary.append(std::to_wstring(
                contracts.dropped_issue_count));
            contract_summary.append(L" source=installed_media_upak read_only=true");
            LogEvent(L"PACKAGE_DDS_CONTRACT_CATALOG_READY", contract_summary);

            auto candidates =
                ds2::modding::BuildGeneralDdsCandidateSnapshot(
                    built.snapshot.get(), contracts.snapshot.get(),
                    std::addressof(mod_index));
            if (!candidates)
            {
                std::wstring detail = L"reason=";
                detail.append(ds2::modding::GeneralDdsCandidateErrorName(
                    candidates.error));
                if (!candidates.detail.empty())
                {
                    detail.append(L" detail=");
                    detail.append(candidates.detail);
                }
                LogEvent(L"GENERAL_DDS_DRY_RUN_FAILED", detail);
                g_package_identity_catalog.store(
                    std::move(built.snapshot), std::memory_order_release);
                return;
            }

            for (const auto& issue : candidates.issues)
            {
                std::wstring detail = L"code=";
                detail.append(
                    ds2::modding::GeneralDdsCandidateIssueCodeName(
                        issue.code));
                detail.append(L" path=");
                detail.append(issue.virtual_path);
                if (!issue.mod_id.empty())
                {
                    detail.append(L" mod=");
                    detail.append(issue.mod_id);
                }
                if (!issue.detail.empty())
                {
                    detail.append(L" detail=");
                    detail.append(issue.detail);
                }
                LogEvent(L"GENERAL_DDS_CANDIDATE_REJECTED", detail);
            }

            for (const auto& candidate : candidates.snapshot->Entries())
            {
                std::wstring detail = L"mod=";
                detail.append(candidate.mod_id);
                detail.append(L" path=");
                detail.append(candidate.virtual_path.key);
                detail.append(L" package_base=");
                AppendHex(detail, candidate.identity.package_base);
                detail.append(L" member_table_offset=");
                AppendHex(detail, candidate.identity.member_table_offset);
                detail.append(L" member_ordinal=");
                detail.append(std::to_wstring(
                    candidate.identity.member_ordinal));
                detail.append(L" full_size=");
                detail.append(std::to_wstring(candidate.original_size));
                detail.append(L" payload_size=");
                detail.append(std::to_wstring(candidate.payload_size));
                detail.append(L" dds=");
                detail.append(std::to_wstring(candidate.dds.width));
                detail.push_back(L'x');
                detail.append(std::to_wstring(candidate.dds.height));
                detail.push_back(L'/');
                detail.append(ds2::modding::DdsFormatName(
                    candidate.dds.format));
                detail.append(L"/mips=");
                detail.append(std::to_wstring(candidate.dds.mip_count));
                detail.append(L" original_full_sha256=");
                detail.append(ds2::modding::Sha256HexWide(
                    candidate.original_full_sha256));
                detail.append(L" original_payload_sha256=");
                detail.append(ds2::modding::Sha256HexWide(
                    candidate.original_payload_sha256));
                detail.append(L" replacement_full_sha256=");
                detail.append(ds2::modding::Sha256HexWide(
                    candidate.replacement_full_sha256));
                detail.append(L" replacement_payload_sha256=");
                detail.append(ds2::modding::Sha256HexWide(
                    candidate.replacement_payload_sha256));
                LogEvent(L"GENERAL_DDS_CANDIDATE", detail);
            }

            std::wstring candidate_summary = L"candidates=";
            candidate_summary.append(std::to_wstring(
                candidates.snapshot->Entries().size()));
            candidate_summary.append(L" rejected=");
            candidate_summary.append(std::to_wstring(candidates.issues.size()));
            candidate_summary.append(L" dropped_rejections=");
            candidate_summary.append(std::to_wstring(
                candidates.dropped_issue_count));
            candidate_summary.append(L" source_hash_gate=sha256 post_write_hash_gate=sha256");
            LogEvent(L"GENERAL_DDS_CATALOG_READY", candidate_summary);

            g_general_dds_candidates.store(
                std::move(candidates.snapshot), std::memory_order_release);
            g_package_identity_catalog.store(
                std::move(built.snapshot), std::memory_order_release);
        }
        catch (...)
        {
            LogEvent(
                L"PACKAGE_IDENTITY_CATALOG_FAILED",
                L"unexpected catalog initialization exception");
        }
    }
#endif

    DWORD WINAPI ResolverEventLogThread(LPVOID) noexcept
    {
        const HANDLE event_handle =
            g_resolver_event_handle.load(std::memory_order_acquire);
        if (event_handle == nullptr)
        {
            return ERROR_INVALID_HANDLE;
        }

        ds2::modding::ResolverProbeStats last_stats{};
        for (;;)
        {
            const DWORD wait_result = WaitForSingleObject(
                event_handle, kResolverStatsIntervalMilliseconds);
            if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT)
            {
                return GetLastError();
            }

            try
            {
                const unsigned long events = g_pending_resolver_events.exchange(
                    0, std::memory_order_acq_rel);
                if ((events & kResolverFirstEntryEvent) != 0)
                {
                    LogEvent(
                        L"INTERNAL_STREAM_FIRST_ENTRY",
                        L"rva=0xABDC0 abi=int(stream,destination,int32)");
                    OutputDebugStringW(
                        L"[Darksiders2DLL] INTERNAL_STREAM_FIRST_ENTRY\n");
                }
                if ((events & kResolverQueueEvent) != 0)
                {
                    ds2::modding::ResolverProbeEvent event;
                    while (ds2::modding::TryPopResolverProbeEvent(event))
                    {
                        LogResolverProbeEvent(event);
                    }
                }

                const auto stats = ds2::modding::GetResolverProbeStats();
                if (!ResolverStatsEqual(stats, last_stats))
                {
                    LogResolverStats(stats);
                    last_stats = stats;
                }
            }
            catch (...)
            {
                LogEvent(
                    L"RESOLVER_EVENT_LOG_FAILED",
                    L"worker formatting or hashing exception");
            }
        }
    }

    void OnResolverProbeEvent(
        const ds2::modding::ResolverProbeEventKind event) noexcept
    {
        const unsigned long flag =
            event == ds2::modding::ResolverProbeEventKind::stream_first_entry
            ? kResolverFirstEntryEvent
            : kResolverQueueEvent;
        g_pending_resolver_events.fetch_or(flag, std::memory_order_release);
        const HANDLE event_handle =
            g_resolver_event_handle.load(std::memory_order_acquire);
        if (event_handle != nullptr)
        {
            SetEvent(event_handle);
        }
    }

    [[nodiscard]] bool StartResolverEventLogger() noexcept
    {
        const HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_handle == nullptr)
        {
            return false;
        }
        g_resolver_event_handle.store(event_handle, std::memory_order_release);

        const HANDLE thread = CreateThread(
            nullptr, 0, ResolverEventLogThread, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            g_resolver_event_handle.store(nullptr, std::memory_order_release);
            CloseHandle(event_handle);
            return false;
        }
        CloseHandle(thread);
        return true;
    }

    [[nodiscard]] bool PinThisModule() noexcept
    {
        HMODULE pinned_module = nullptr;
        return GetModuleHandleExW(
                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_PIN,
                   reinterpret_cast<LPCWSTR>(std::addressof(g_bootstrapStatus)),
                   &pinned_module) != FALSE;
    }

    void RunFramework() noexcept
    {
        try
        {
            if (!PinThisModule())
            {
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] MODULE_PIN_FAILED\n");
                return;
            }

            const auto log_directory = GetLogDirectory();
            if (!log_directory)
            {
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::logging_initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] LOGGING_INITIALIZATION_FAILED\n");
                return;
            }

            auto logger_result = ds2::modding::OpenSessionLogger(*log_directory);
            if (!logger_result)
            {
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::logging_initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] LOGGING_INITIALIZATION_FAILED\n");
                return;
            }
            g_logger.store(std::move(logger_result.logger), std::memory_order_release);
            LogEvent(L"SESSION_START", L"version=0.4.0 mode=dds_loader");

            const auto build = ds2::modding::IdentifyCurrentGameBuild();
            std::wstring build_detail = L"path=";
            build_detail.append(build.executable_path.native());
            build_detail.append(L" size=");
            build_detail.append(std::to_wstring(build.file_size));
            if (!build.sha256_hex.empty())
            {
                build_detail.append(L" sha256=");
                build_detail.append(build.sha256_hex);
            }
            if (!build.Supported())
            {
                build_detail.append(L" reason=");
                build_detail.append(ds2::modding::GameBuildErrorName(build.error));
                LogEvent(L"BUILD_UNSUPPORTED", build_detail);
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::unsupported_build,
                    std::memory_order_release);
                return;
            }

            LogEvent(L"BUILD_SUPPORTED", build_detail);
            const auto game_directory = build.executable_path.parent_path();
            const HMODULE game_module = GetModuleHandleW(nullptr);
            g_game_image_base.store(
                reinterpret_cast<std::uintptr_t>(game_module),
                std::memory_order_release);
            const auto config = ds2::modding::LoadLoaderConfig(game_directory);
            if (!config.valid || !config.config.enabled) {
                LogEvent(config.valid ? L"LOADER_DISABLED" : L"CONFIG_REJECTED", config.error);
                g_bootstrapStatus.store(ds2::bootstrap::Status::ready_proxy_only,
                                        std::memory_order_release);
                return;
            }
            LogEvent(L"LOADER_CONFIG", config.config.write_enabled ? L"mode=override" : L"mode=observe");
            ds2::modding::ModIndexOptions index_options;
            index_options.dds_only = true;
            index_options.max_asset_file_size = 64ull * 1024 * 1024;
            index_options.max_total_bytes = 256ull * 1024 * 1024;
            auto index_result = ds2::modding::BuildModIndex(game_directory / L"mods", index_options);
            if (!index_result)
            {
                std::wstring detail = L"reason=";
                detail.append(ds2::modding::ModIndexErrorName(index_result.error));
                detail.append(L" win32=");
                detail.append(std::to_wstring(index_result.system_error));
                LogEvent(L"MOD_INDEX_FAILED", detail);
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::mod_index_initialization_failed,
                    std::memory_order_release);
                return;
            }

            for (const auto& mod_id : index_result.snapshot->ModIds())
            {
                LogEvent(L"MOD_INDEXED", mod_id);
            }
            for (const auto& issue : index_result.issues)
            {
                std::wstring detail = L"code=";
                detail.append(ds2::modding::ModIndexIssueCodeName(issue.code));
                if (!issue.mod_id.empty())
                {
                    detail.append(L" mod=");
                    detail.append(issue.mod_id);
                }
                detail.append(L" path=");
                detail.append(issue.path.native());
                LogEvent(L"MOD_INDEX_ISSUE", detail);
            }

            auto snapshot = std::move(index_result.snapshot);
            InitializeDdsCatalog(game_directory, *snapshot);
            g_mod_index.store(snapshot, std::memory_order_release);
            const auto candidates = g_general_dds_candidates.load(std::memory_order_acquire);
            if (!candidates || candidates->Entries().empty()) {
                LogEvent(L"ASSET_FALLBACK", L"no compatible DDS candidates");
                g_bootstrapStatus.store(ds2::bootstrap::Status::ready_proxy_only,
                                        std::memory_order_release);
                return;
            }

            bool resolver_probe_active = false;
            if (!StartResolverEventLogger())
            {
                LogEvent(L"RESOLVER_EVENT_LOGGER_FAILED");
            }
            else
            {
                ds2::modding::SetResolverProbeEventCallback(OnResolverProbeEvent);
                resolver_probe_active = ds2::modding::InitializeResolverProbe(
                    game_module,
                    game_directory / L"media" / L"media.upak",
                    candidates, config.config.write_enabled);
                const auto probe_status = ds2::modding::GetResolverProbeStatus();
                std::wstring probe_detail = L"read_file=";
                probe_detail.append(BoolName(probe_status.read_file_hook_installed));
                probe_detail.append(L" close_handle=");
                probe_detail.append(BoolName(probe_status.close_handle_hook_installed));
                probe_detail.append(L" stream_read=");
                probe_detail.append(BoolName(probe_status.stream_read_hook_installed));
                probe_detail.append(L" stream_rva=");
                AppendHex(probe_detail, probe_status.stream_read_rva);
#if defined(DS2_RESOURCE_IDENTITY_ENABLED)
                probe_detail.append(L" resource_identity=");
                probe_detail.append(BoolName(
                    probe_status.resource_identity_hook_installed));
                probe_detail.append(L" resource_identity_rva=");
                AppendHex(probe_detail, probe_status.resource_identity_rva);
#endif
                probe_detail.append(L" image_base=");
                AppendHex(
                    probe_detail,
                    reinterpret_cast<std::uintptr_t>(game_module));
                probe_detail.append(L" minhook_status=");
                probe_detail.append(std::to_wstring(probe_status.last_minhook_status));
                probe_detail.append(config.config.write_enabled
                    ? L" override=verified_general_sha256" : L" override=observe_only");
                LogEvent(
                    resolver_probe_active
                        ? L"INTERNAL_ASSET_OVERRIDE_ACTIVE"
                        : L"RESOLVER_PROBE_FAILED",
                    probe_detail);
            }

            g_bootstrapStatus.store(
                resolver_probe_active ? ds2::bootstrap::Status::ready_internal_override
                                      : ds2::bootstrap::Status::ready_proxy_only,
                std::memory_order_release);

        }
        catch (...)
        {
            const bool resolver_probe_active =
                ds2::modding::GetResolverProbeStatus().state ==
                ds2::modding::ResolverProbeState::active;
            if (resolver_probe_active)
            {
                LogEvent(
                    L"INITIALIZATION_WARNING",
                    L"unhandled C++ exception after hook activation");
                g_bootstrapStatus.store(
                    resolver_probe_active
                        ? ds2::bootstrap::Status::ready_internal_override
                        : ds2::bootstrap::Status::ready_texture_override,
                    std::memory_order_release);
                OutputDebugStringW(
                    L"[Darksiders2DLL] INITIALIZATION_WARNING_AFTER_TEXTURE_HOOK_ACTIVE\n");
            }
            else
            {
                LogEvent(L"INITIALIZATION_FAILED", L"unhandled C++ exception");
                g_bootstrapStatus.store(
                    ds2::bootstrap::Status::initialization_failed,
                    std::memory_order_release);
                OutputDebugStringW(L"[Darksiders2DLL] INITIALIZATION_FAILED\n");
            }
        }
    }

    DWORD WINAPI BootstrapThread(LPVOID) noexcept
    {
        RunFramework();
        return 0;
    }

    BOOL CALLBACK ScheduleFramework(
        PINIT_ONCE initOnce,
        PVOID parameter,
        PVOID* context) noexcept
    {
        UNREFERENCED_PARAMETER(initOnce);
        UNREFERENCED_PARAMETER(parameter);
        UNREFERENCED_PARAMETER(context);

        g_bootstrapStatus.store(ds2::bootstrap::Status::starting, std::memory_order_release);
        const HANDLE thread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
            g_bootstrapStatus.store(
                ds2::bootstrap::Status::initialization_failed,
                std::memory_order_release);
            OutputDebugStringW(L"[Darksiders2DLL] BOOTSTRAP_THREAD_FAILED\n");
            // Leave INIT_ONCE incomplete so a later DirectInput8Create call
            // can retry scheduling after a transient thread-creation failure.
            return FALSE;
        }
        CloseHandle(thread);
        return TRUE;
    }
}

namespace ds2::bootstrap
{
    Status EnsureInitialized() noexcept
    {
        static_cast<void>(InitOnceExecuteOnce(
            &g_bootstrapInitOnce,
            ScheduleFramework,
            nullptr,
            nullptr));

        return CurrentStatus();
    }

    Status CurrentStatus() noexcept
    {
        return g_bootstrapStatus.load(std::memory_order_acquire);
    }

    const wchar_t* StatusName(const Status status) noexcept
    {
        switch (status)
        {
        case Status::not_started: return L"not_started";
        case Status::starting: return L"starting";
        case Status::ready_proxy_only: return L"ready_proxy_only";
        case Status::ready_texture_override: return L"ready_texture_override";
        case Status::ready_internal_override: return L"ready_internal_override";
        case Status::unsupported_build: return L"unsupported_build";
        case Status::logging_initialization_failed: return L"logging_initialization_failed";
        case Status::mod_index_initialization_failed: return L"mod_index_initialization_failed";
        case Status::texture_hook_initialization_failed: return L"texture_hook_initialization_failed";
        case Status::initialization_failed: return L"initialization_failed";
        }
        return L"unknown";
    }
}
