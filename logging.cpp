#include "logging.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace ds2::modding {
namespace {

constexpr wchar_t kReplacementCharacter = static_cast<wchar_t>(0xFFFD);

class FileHandle final {
public:
    explicit FileHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~FileHandle() { Reset(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }

private:
    void Reset() noexcept {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
            value_ = INVALID_HANDLE_VALUE;
        }
    }
    HANDLE value_;
};

bool IsHighSurrogate(const wchar_t value) noexcept {
    return value >= static_cast<wchar_t>(0xD800) && value <= static_cast<wchar_t>(0xDBFF);
}

bool IsLowSurrogate(const wchar_t value) noexcept {
    return value >= static_cast<wchar_t>(0xDC00) && value <= static_cast<wchar_t>(0xDFFF);
}

std::wstring SanitizeText(const std::wstring_view input) {
    std::wstring output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const wchar_t value = input[index];
        if (value == L'\r') {
            output.append(L"\\r");
        } else if (value == L'\n') {
            output.append(L"\\n");
        } else if (value == L'\t') {
            output.append(L"\\t");
        } else if (value < 0x20 || value == 0x7F) {
            output.push_back(kReplacementCharacter);
        } else if (IsHighSurrogate(value)) {
            if (index + 1 < input.size() && IsLowSurrogate(input[index + 1])) {
                output.push_back(value);
                output.push_back(input[++index]);
            } else {
                output.push_back(kReplacementCharacter);
            }
        } else if (IsLowSurrogate(value)) {
            output.push_back(kReplacementCharacter);
        } else {
            output.push_back(value);
        }
    }
    return output;
}

bool WideToUtf8(const std::wstring_view input, std::string& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(size));
    return WideCharToMultiByte(
               CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
               output.data(), size, nullptr, nullptr) == size;
}

bool WriteAll(const HANDLE file, const std::string_view bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool IsSafeStem(const std::wstring_view stem) noexcept {
    if (stem.empty() || stem == L"." || stem == L"..") {
        return false;
    }
    for (const wchar_t value : stem) {
        if (value < 0x20 || value == L'<' || value == L'>' || value == L':' ||
            value == L'"' || value == L'/' || value == L'\\' || value == L'|' ||
            value == L'?' || value == L'*') {
            return false;
        }
    }
    return stem.back() != L'.' && stem.back() != L' ';
}

}  // namespace

struct SessionLogger::Impl final {
    FileHandle file;
    std::filesystem::path path;
    std::mutex mutex;
    std::size_t bytes_written{};
};

SessionLogger::SessionLogger(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

SessionLogger::~SessionLogger() = default;

LoggerOpenResult OpenSessionLogger(
    const std::filesystem::path& log_directory,
    const std::wstring_view file_stem) {
    LoggerOpenResult result;
    if (log_directory.empty() || !IsSafeStem(file_stem)) {
        result.win32_error = ERROR_INVALID_PARAMETER;
        result.detail = L"invalid log directory or file stem";
        return result;
    }

    std::error_code error;
    std::filesystem::create_directories(log_directory, error);
    if (error) {
        result.win32_error = static_cast<unsigned long>(error.value());
        result.detail = L"cannot create log directory";
        return result;
    }
    const DWORD attributes = GetFileAttributesW(log_directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.win32_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_REPARSE_TAG_INVALID;
        result.detail = L"log directory is unavailable or is a reparse point";
        return result;
    }

    SYSTEMTIME time{};
    GetSystemTime(&time);
    const DWORD process_id = GetCurrentProcessId();
    std::unique_ptr<SessionLogger::Impl> implementation;
    try {
        implementation = std::make_unique<SessionLogger::Impl>();
    } catch (const std::bad_alloc&) {
        result.win32_error = ERROR_NOT_ENOUGH_MEMORY;
        result.detail = L"cannot allocate logger";
        return result;
    }

    for (unsigned int attempt = 0; attempt != 1000; ++attempt) {
        std::array<wchar_t, 256> name{};
        const int length = swprintf_s(
            name.data(), name.size(), L"%.*s-%04u%02u%02u-%02u%02u%02u-%lu-%03u.log",
            static_cast<int>(file_stem.size()), file_stem.data(), time.wYear, time.wMonth,
            time.wDay, time.wHour, time.wMinute, time.wSecond,
            static_cast<unsigned long>(process_id), attempt);
        if (length <= 0) {
            result.win32_error = ERROR_INVALID_NAME;
            result.detail = L"log file name is too long";
            return result;
        }
        const auto candidate = log_directory / name.data();
        FileHandle file(CreateFileW(
            candidate.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (file.Get() != INVALID_HANDLE_VALUE) {
            implementation->file = std::move(file);
            implementation->path = candidate;
            try {
                result.logger = std::shared_ptr<SessionLogger>(
                    new SessionLogger(std::move(implementation)));
            } catch (const std::bad_alloc&) {
                result.win32_error = ERROR_NOT_ENOUGH_MEMORY;
                result.detail = L"cannot allocate logger owner";
            }
            return result;
        }
        const DWORD open_error = GetLastError();
        if (open_error != ERROR_FILE_EXISTS && open_error != ERROR_ALREADY_EXISTS) {
            result.win32_error = open_error;
            result.detail = L"cannot create session log";
            return result;
        }
    }
    result.win32_error = ERROR_FILE_EXISTS;
    result.detail = L"cannot choose a unique session log name";
    return result;
}

bool SessionLogger::Write(const LogLevel level, const std::wstring_view message) noexcept {
    try {
        SYSTEMTIME time{};
        GetSystemTime(&time);
        std::array<wchar_t, 64> prefix{};
        const int prefix_length = swprintf_s(
            prefix.data(), prefix.size(), L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%.*s] ",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
            time.wMilliseconds, static_cast<int>(LogLevelName(level).size()),
            LogLevelName(level).data());
        if (prefix_length <= 0) {
            return false;
        }
        std::wstring line(prefix.data(), static_cast<std::size_t>(prefix_length));
        line.append(SanitizeText(message));
        line.push_back(L'\n');
        std::string utf8;
        if (!WideToUtf8(line, utf8)) {
            return false;
        }
        std::scoped_lock lock(implementation_->mutex);
        constexpr std::size_t max_log_bytes = 16 * 1024 * 1024;
        if (utf8.size() > max_log_bytes - implementation_->bytes_written) return false;
        // Charge attempted bytes too, so partial writes cannot evade the cap.
        implementation_->bytes_written += utf8.size();
        return WriteAll(implementation_->file.Get(), utf8);
    } catch (...) {
        return false;
    }
}

bool SessionLogger::Event(
    const std::wstring_view event_name,
    const std::wstring_view detail) noexcept {
    try {
        std::wstring message(event_name);
        if (!detail.empty()) {
            message.push_back(L' ');
            message.append(detail);
        }
        return Write(LogLevel::info, message);
    } catch (...) {
        return false;
    }
}

const std::filesystem::path& SessionLogger::Path() const noexcept {
    return implementation_->path;
}

std::wstring_view LogLevelName(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::debug: return L"DEBUG";
    case LogLevel::info: return L"INFO";
    case LogLevel::warning: return L"WARN";
    case LogLevel::error: return L"ERROR";
    }
    return L"UNKNOWN";
}

}  // namespace ds2::modding
