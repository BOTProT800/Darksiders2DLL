#pragma once

#include <cstddef>

namespace ds2::modding {

// Verifies that every page touched by [data, data + size) is committed and
// readable at the time of the query. This is a preflight check: callers must
// still avoid retaining pointers whose lifetime they do not control.
[[nodiscard]] bool IsReadableMemoryRange(
    const void* data,
    std::size_t size) noexcept;

// Equivalent page-by-page preflight requiring writable protection. It does
// not extend the lifetime of a caller-owned buffer.
[[nodiscard]] bool IsWritableMemoryRange(
    const void* data,
    std::size_t size) noexcept;

}  // namespace ds2::modding
