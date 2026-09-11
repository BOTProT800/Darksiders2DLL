#pragma once

namespace ds2::bootstrap
{
    enum class Status
    {
        not_started,
        starting,
        ready_proxy_only,
        ready_texture_override,
        unsupported_build,
        logging_initialization_failed,
        mod_index_initialization_failed,
        texture_hook_initialization_failed,
        initialization_failed,
    };

    // Idempotently schedules a worker and never waits for it. It is safe for
    // DllMain to request scheduling because fingerprinting, filesystem I/O,
    // logging and MinHook setup all remain on the worker, outside loader
    // initialization. DirectInput8Create calls it again as a retry point.
    // Failure is deliberately non-fatal: DirectInput must still be forwarded.
    [[nodiscard]] Status EnsureInitialized() noexcept;
    [[nodiscard]] Status CurrentStatus() noexcept;
    [[nodiscard]] const wchar_t* StatusName(Status status) noexcept;
}
