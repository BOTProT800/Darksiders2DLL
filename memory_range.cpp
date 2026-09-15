#include "pch.h"

#include "memory_range.h"

#include <cstdint>
#include <limits>

namespace ds2::modding {
namespace {

[[nodiscard]] bool IsReadableProtection(const DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    switch (protection & 0xFFu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool IsWritableProtection(const DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    switch (protection & 0xFFu) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

template <typename ProtectionPredicate>
[[nodiscard]] bool IsMemoryRangeWithProtection(
    const void* const data,
    const std::size_t size,
    ProtectionPredicate predicate) noexcept {
    if (data == nullptr || size == 0) {
        return false;
    }

    const auto first = reinterpret_cast<std::uintptr_t>(data);
    constexpr auto kMaximumAddress = (std::numeric_limits<std::uintptr_t>::max)();
    if (size > kMaximumAddress - first) {
        return false;
    }
    const auto end = first + size;

    auto current = first;
    while (current < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(current),
                &memory,
                sizeof(memory)) != sizeof(memory)) {
            return false;
        }
        if (memory.State != MEM_COMMIT || !predicate(memory.Protect)) {
            return false;
        }

        const auto region_first =
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > kMaximumAddress - region_first) {
            return false;
        }
        const auto region_end = region_first + memory.RegionSize;
        if (current < region_first || region_end <= current) {
            return false;
        }
        current = region_end < end ? region_end : end;
    }

    return true;
}

}  // namespace

bool IsReadableMemoryRange(
    const void* const data,
    const std::size_t size) noexcept {
    return IsMemoryRangeWithProtection(data, size, IsReadableProtection);
}

bool IsWritableMemoryRange(
    const void* const data,
    const std::size_t size) noexcept {
    return IsMemoryRangeWithProtection(data, size, IsWritableProtection);
}

}  // namespace ds2::modding
