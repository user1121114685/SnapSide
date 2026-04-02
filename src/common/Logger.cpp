#include "common/Logger.h"

#include <array>
#include <cwchar>
#include <mutex>
#include <string>
#include <string_view>

#include <ShlObj.h>

#include "IconMover/Version.h"
#include "common/Win32Error.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kLogFileName[] = L"icon_mover.log";
constexpr wchar_t kDefaultPhase[] = L"general";
constexpr std::array<unsigned char, 3> kUtf8Bom = {0xEF, 0xBB, 0xBF};

struct LoggerState
{
    std::mutex mutex;
    std::wstring log_directory;
    std::wstring log_file_path;
    bool initialized = false;
    bool available = false;
};

LoggerState& GetLoggerState()
{
    static LoggerState state;
    return state;
}

const wchar_t* ToString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Info:
        return L"INFO";

    case LogLevel::Warning:
        return L"WARNING";

    case LogLevel::Error:
        return L"ERROR";

    default:
        return L"UNKNOWN";
    }
}

void OutputLoggerFailure(std::wstring_view failure)
{
    std::wstring message = L"IconMover logger: ";
    message.append(failure.data(), failure.size());
    message += L"\r\n";
    ::OutputDebugStringW(message.c_str());
}

std::wstring BuildTimestamp()
{
    SYSTEMTIME local_time = {};
    ::GetLocalTime(&local_time);

    wchar_t buffer[64] = {};
    swprintf_s(
        buffer,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        local_time.wYear,
        local_time.wMonth,
        local_time.wDay,
        local_time.wHour,
        local_time.wMinute,
        local_time.wSecond,
        local_time.wMilliseconds);

    return buffer;
}

std::wstring BuildLogEntry(LogLevel level, std::wstring_view phase, std::wstring_view message)
{
    const std::wstring_view resolved_phase = phase.empty() ? std::wstring_view(kDefaultPhase) : phase;

    std::wstring entry;
    entry.reserve(64 + resolved_phase.size() + message.size());
    entry += BuildTimestamp();
    entry += L" | ";
    entry += ToString(level);
    entry += L" | ";
    entry.append(resolved_phase.data(), resolved_phase.size());
    entry += L" | ";
    entry.append(message.data(), message.size());
    return entry;
}

std::wstring BuildLogDirectoryPath()
{
    wchar_t app_data_path[MAX_PATH] = {};
    const HRESULT result = ::SHGetFolderPathW(
        nullptr,
        CSIDL_APPDATA | CSIDL_FLAG_CREATE,
        nullptr,
        SHGFP_TYPE_CURRENT,
        app_data_path);

    if (FAILED(result))
    {
        return std::wstring();
    }

    std::wstring path = app_data_path;
    if (!path.empty() && path.back() != L'\\')
    {
        path += L'\\';
    }

    path += kApplicationName;
    return path;
}

bool EnsureDirectoryExists(const std::wstring& path)
{
    if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE)
    {
        return true;
    }

    const DWORD error_code = ::GetLastError();
    if (error_code != ERROR_ALREADY_EXISTS)
    {
        return false;
    }

    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        ::SetLastError(ERROR_DIRECTORY);
        return false;
    }

    return true;
}

bool WriteAll(HANDLE handle, const void* buffer, DWORD byte_count)
{
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    DWORD total_written = 0;

    while (total_written < byte_count)
    {
        DWORD chunk_written = 0;
        if (::WriteFile(handle, bytes + total_written, byte_count - total_written, &chunk_written, nullptr) == FALSE)
        {
            return false;
        }

        total_written += chunk_written;
    }

    return true;
}

bool EnsureUtf8Bom(HANDLE handle)
{
    LARGE_INTEGER file_size = {};
    if (::GetFileSizeEx(handle, &file_size) == FALSE)
    {
        return false;
    }

    if (file_size.QuadPart != 0)
    {
        return true;
    }

    return WriteAll(handle, kUtf8Bom.data(), static_cast<DWORD>(kUtf8Bom.size()));
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty())
    {
        return std::string();
    }

    const int required_size = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required_size <= 0)
    {
        return std::string();
    }

    std::string converted(static_cast<size_t>(required_size), '\0');
    ::WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        converted.data(),
        required_size,
        nullptr,
        nullptr);

    return converted;
}

bool EnsureLogFileExists(const std::wstring& path)
{
    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const bool succeeded = EnsureUtf8Bom(handle);
    ::CloseHandle(handle);
    return succeeded;
}

bool AppendLineToFile(const std::wstring& path, std::wstring_view line)
{
    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    if (!EnsureUtf8Bom(handle))
    {
        const DWORD error_code = ::GetLastError();
        ::CloseHandle(handle);
        ::SetLastError(error_code);
        return false;
    }

    LARGE_INTEGER end_of_file = {};
    if (::SetFilePointerEx(handle, end_of_file, nullptr, FILE_END) == FALSE)
    {
        const DWORD error_code = ::GetLastError();
        ::CloseHandle(handle);
        ::SetLastError(error_code);
        return false;
    }

    std::wstring terminated_line(line);
    terminated_line += L"\r\n";

    const std::string utf8_line = WideToUtf8(terminated_line);
    if (!terminated_line.empty() && utf8_line.empty())
    {
        ::CloseHandle(handle);
        ::SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return false;
    }

    const bool succeeded = WriteAll(handle, utf8_line.data(), static_cast<DWORD>(utf8_line.size()));
    const DWORD error_code = succeeded ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);

    if (!succeeded)
    {
        ::SetLastError(error_code);
    }

    return succeeded;
}
}  // namespace

bool Logger::Initialize()
{
    const DWORD preserved_error = ::GetLastError();
    bool available = false;
    bool emit_failure = false;
    std::wstring failure_message;

    auto& state = GetLoggerState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);

        if (!state.initialized)
        {
            state.initialized = true;
            state.available = false;
            state.log_directory.clear();
            state.log_file_path.clear();

            const std::wstring log_directory = BuildLogDirectoryPath();
            if (log_directory.empty())
            {
                failure_message = L"Failed to resolve %APPDATA% for log initialization.";
            }
            else if (!EnsureDirectoryExists(log_directory))
            {
                failure_message = L"Failed to create log directory. " + DescribeLastWin32Error();
            }
            else
            {
                const std::wstring log_file_path = log_directory + L'\\' + kLogFileName;
                if (!EnsureLogFileExists(log_file_path))
                {
                    failure_message = L"Failed to create log file. " + DescribeLastWin32Error();
                }
                else
                {
                    state.log_directory = log_directory;
                    state.log_file_path = log_file_path;
                    state.available = true;
                }
            }

            emit_failure = !state.available;
        }

        available = state.available;
    }

    if (emit_failure)
    {
        OutputLoggerFailure(failure_message);
    }

    ::SetLastError(preserved_error);
    return available;
}

void Logger::Write(LogLevel level, std::wstring_view phase, std::wstring_view message)
{
    const DWORD preserved_error = ::GetLastError();
    if (!Initialize())
    {
        ::SetLastError(preserved_error);
        return;
    }

    const std::wstring entry = BuildLogEntry(level, phase, message);

    auto& state = GetLoggerState();
    bool wrote_entry = false;
    DWORD write_error = ERROR_SUCCESS;

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.available)
        {
            wrote_entry = AppendLineToFile(state.log_file_path, entry);
            if (!wrote_entry)
            {
                write_error = ::GetLastError();
            }
        }
    }

    if (!wrote_entry)
    {
        OutputLoggerFailure(L"Failed to write log entry. " + DescribeWin32Error(write_error));
    }

    ::SetLastError(preserved_error);
}

void Logger::Info(std::wstring_view phase, std::wstring_view message)
{
    Write(LogLevel::Info, phase, message);
}

void Logger::Warning(std::wstring_view phase, std::wstring_view message)
{
    Write(LogLevel::Warning, phase, message);
}

void Logger::Error(std::wstring_view phase, std::wstring_view message)
{
    Write(LogLevel::Error, phase, message);
}

void Logger::LogLastError(LogLevel level, std::wstring_view phase, std::wstring_view context)
{
    const DWORD error_code = ::GetLastError();

    std::wstring message(context);
    if (!message.empty())
    {
        message += L' ';
    }

    message += DescribeWin32Error(error_code);
    Write(level, phase, message);
}

std::wstring Logger::GetLogFilePath()
{
    auto& state = GetLoggerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.log_file_path;
}
}  // namespace icon_mover
