#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ds2::modding {

inline constexpr std::size_t kResourceIdentityMaxDepth = 8;
inline constexpr std::uint32_t kResourceIdentityMaxReadsPerScope = 65'536;

// Values passed into the correlator must already have been copied and
// validated by the hook. The correlator never dereferences game pointers.
struct ResourceIdentityScopeInput final {
    std::uintptr_t owner{};
    std::uintptr_t argument2{};
    std::uintptr_t argument3{};
    std::uintptr_t argument4{};
    std::uintptr_t argument5{};
    // Package-side object obtained from the fifth argument. This is distinct
    // from the stack-local stream wrapper passed to the inner read call.
    std::uintptr_t stream{};
    std::uint64_t package_base{};
    std::int32_t member_table_offset{};
    bool object_fields_valid{};
    bool stream_valid{};
    std::uintptr_t caller_rva{};
};

struct ResourceIdentityReadInput final {
    std::uintptr_t stream{};
    std::uintptr_t destination{};
    std::uintptr_t caller_rva{};
    std::int32_t requested{};
    std::int32_t returned{};
    bool hash_valid{};
    std::array<std::uint8_t, 32> sha256{};
};

struct ResourceIdentityScopeToken final {
    std::uint64_t sequence{};
    std::size_t nesting_depth{};
    bool tracked{};
};

enum class ResourceIdentityBeginStatus {
    tracked,
    suppressed_overflow,
};

struct ResourceIdentityBeginResult final {
    ResourceIdentityBeginStatus status{ResourceIdentityBeginStatus::tracked};
    ResourceIdentityScopeToken token{};
};

enum class ResourceIdentityObserveStatus {
    recorded,
    no_active_scope,
    suppressed_overflow,
    invalid_scope_stream,
    invalid_read_stream,
    read_limit_reached,
};

enum class ResourceIdentityEndStatus {
    matched,
    suppressed_overflow_unwound,
    context_reset,
};

// POD emitted by the optional Debug diagnostic hook. It identifies the innermost
// resource scope and numbers only non-null reads already filtered by the hook
// to the supported static call site. The package-side object and the inner
// stack-local stream wrapper are intentionally distinct. Rejected reads never
// consume an ordinal.
struct ResourceIdentitySample final {
    std::uint64_t sequence{};
    std::uint32_t nesting_depth{};
    std::uint32_t read_ordinal{};
    ResourceIdentityScopeInput scope{};
    ResourceIdentityReadInput read{};
};

static_assert(std::is_trivially_copyable_v<ResourceIdentityScopeInput>);
static_assert(std::is_trivially_copyable_v<ResourceIdentityReadInput>);
static_assert(std::is_trivially_copyable_v<ResourceIdentityScopeToken>);
static_assert(std::is_trivially_copyable_v<ResourceIdentitySample>);

// Pure, allocation-free final gate for identity-based decisions. The caller
// supplies the two static return RVAs proven for the supported executable.
[[nodiscard]] bool IsResourceIdentitySampleUsable(
    const ResourceIdentitySample& sample,
    std::uintptr_t expected_scope_caller_rva,
    std::uintptr_t expected_read_caller_rva) noexcept;

// One instance belongs to one thread. It is fixed-size and allocation-free so
// a detour can use it as thread-local state. Any out-of-order scope close
// clears the context instead of risking a false resource association.
class ResourceIdentityThreadContext final {
public:
    constexpr ResourceIdentityThreadContext() noexcept = default;

    [[nodiscard]] ResourceIdentityBeginResult Begin(
        const ResourceIdentityScopeInput& input,
        std::uint64_t sequence) noexcept;

    [[nodiscard]] ResourceIdentityObserveStatus ObserveRead(
        const ResourceIdentityReadInput& input,
        ResourceIdentitySample& sample) noexcept;

    [[nodiscard]] ResourceIdentityEndStatus End(
        const ResourceIdentityScopeToken& token) noexcept;

    void Reset() noexcept;

    [[nodiscard]] std::size_t ActiveDepth() const noexcept;
    [[nodiscard]] std::size_t SuppressedDepth() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;

private:
    struct Frame final {
        ResourceIdentityScopeInput input{};
        std::uint64_t sequence{};
        std::uint32_t read_count{};
    };

    std::array<Frame, kResourceIdentityMaxDepth> frames_{};
    std::size_t depth_{};
    std::size_t suppressed_depth_{};
};

}  // namespace ds2::modding
