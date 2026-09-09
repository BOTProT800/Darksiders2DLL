#pragma once

namespace ds2::bootstrap
{
    enum class Status
    {
        not_started,
        ready,
        minhook_initialization_failed,
        initialization_failed,
    };

    // Called lazily by DirectInput8Create, never from DllMain. Failure is
    // deliberately non-fatal: the proxy must continue forwarding DirectInput.
    [[nodiscard]] Status EnsureInitialized() noexcept;
    [[nodiscard]] Status CurrentStatus() noexcept;
}
