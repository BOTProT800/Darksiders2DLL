#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace ds2::modding {

enum class LogLevel {
    debug,
    info,
    warning,
    error,
};

class SessionLogger;

struct LoggerOpenResult final {
    std::shared_ptr<SessionLogger> logger;
    unsigned long win32_error{};
    std::wstring detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return logger != nullptr;
    }
};

// Opens a new, process-specific UTF-8 log. Existing files are never appended
// to or overwritten. The directory is created when it does not yet exist.
[[nodiscard]] LoggerOpenResult OpenSessionLogger(
    const std::filesystem::path& log_directory,
    std::wstring_view file_stem = L"Darksiders2DLL");

class SessionLogger final {
public:
    ~SessionLogger();
    SessionLogger(const SessionLogger&) = delete;
    SessionLogger& operator=(const SessionLogger&) = delete;

    // Invalid UTF-16 is replaced with U+FFFD. CR/LF and control characters in
    // caller text are escaped so a value cannot forge an additional log line.
    [[nodiscard]] bool Write(LogLevel level, std::wstring_view message) noexcept;
    [[nodiscard]] bool Event(
        std::wstring_view event_name,
        std::wstring_view detail = {}) noexcept;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept;

private:
    struct Impl;
    explicit SessionLogger(std::unique_ptr<Impl> implementation) noexcept;
    friend LoggerOpenResult OpenSessionLogger(
        const std::filesystem::path&, std::wstring_view);

    std::unique_ptr<Impl> implementation_;
};

[[nodiscard]] std::wstring_view LogLevelName(LogLevel level) noexcept;

}  // namespace ds2::modding
