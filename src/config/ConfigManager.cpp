#include "config/ConfigManager.h"

#include <array>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>
#include <ShlObj.h>

#include "IconMover/Version.h"
#include "common/Logger.h"
#include "common/Win32Error.h"
#include "core/Constants.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kConfigPhase[] = L"config";
constexpr std::array<unsigned char, 3> kUtf8Bom = {0xEF, 0xBB, 0xBF};
constexpr ULONGLONG kWindowsToUnixEpochTicks = 116444736000000000ULL;

const wchar_t* ToConfigValue(TrayMenuLanguage tray_menu_language) noexcept
{
    switch (tray_menu_language)
    {
    case TrayMenuLanguage::ChineseSimplified:
        return L"zh-CN";

    case TrayMenuLanguage::English:
    default:
        return L"en";
    }
}

bool EqualsCaseInsensitive(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (size_t index = 0; index < left.size(); ++index)
    {
        if (towlower(left[index]) != towlower(right[index]))
        {
            return false;
        }
    }

    return true;
}

bool TryParseTrayMenuLanguage(std::wstring_view value, TrayMenuLanguage& tray_menu_language)
{
    if (EqualsCaseInsensitive(value, L"en") || EqualsCaseInsensitive(value, L"english"))
    {
        tray_menu_language = TrayMenuLanguage::English;
        return true;
    }

    if (EqualsCaseInsensitive(value, L"zh") ||
        EqualsCaseInsensitive(value, L"zh-cn") ||
        EqualsCaseInsensitive(value, L"zh-hans") ||
        EqualsCaseInsensitive(value, L"chinese"))
    {
        tray_menu_language = TrayMenuLanguage::ChineseSimplified;
        return true;
    }

    return false;
}

AppConfig BuildDefaultConfig()
{
    return AppConfig{};
}

void NormalizeConfig(AppConfig& config)
{
    if (config.log_level.empty())
    {
        config.log_level = L"info";
    }
}

std::wstring_view Trim(std::wstring_view value)
{
    while (!value.empty() && iswspace(value.front()) != 0)
    {
        value.remove_prefix(1);
    }

    while (!value.empty() && iswspace(value.back()) != 0)
    {
        value.remove_suffix(1);
    }

    return value;
}

bool HasMeaningfulContent(std::wstring_view value)
{
    for (const wchar_t character : value)
    {
        if (iswspace(character) == 0)
        {
            return true;
        }
    }

    return false;
}

void AppendIssue(std::wstring& issues, std::wstring_view issue)
{
    if (issue.empty())
    {
        return;
    }

    if (!issues.empty())
    {
        issues += L"; ";
    }

    issues.append(issue.data(), issue.size());
}

std::wstring BuildConfigDirectoryPath()
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

bool ReadAll(HANDLE handle, std::string& content)
{
    LARGE_INTEGER file_size = {};
    if (::GetFileSizeEx(handle, &file_size) == FALSE)
    {
        return false;
    }

    if (file_size.QuadPart < 0 || file_size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<DWORD>::max()))
    {
        ::SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }

    content.assign(static_cast<size_t>(file_size.QuadPart), '\0');
    DWORD total_read = 0;
    while (total_read < content.size())
    {
        DWORD chunk_read = 0;
        if (::ReadFile(
                handle,
                content.data() + total_read,
                static_cast<DWORD>(content.size() - total_read),
                &chunk_read,
                nullptr) == FALSE)
        {
            return false;
        }

        if (chunk_read == 0)
        {
            break;
        }

        total_read += chunk_read;
    }

    content.resize(total_read);
    return true;
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

bool Utf8ToWide(std::string_view value, std::wstring& converted)
{
    converted.clear();
    if (value.empty())
    {
        return true;
    }

    const int required_size = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);

    if (required_size <= 0)
    {
        return false;
    }

    converted.resize(static_cast<size_t>(required_size));
    const int converted_size = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        converted.data(),
        required_size);

    return converted_size == required_size;
}

bool ReadUtf8TextFile(const std::wstring& path, std::wstring& content)
{
    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    std::string bytes;
    const bool read_succeeded = ReadAll(handle, bytes);
    const DWORD error_code = read_succeeded ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);

    if (!read_succeeded)
    {
        ::SetLastError(error_code);
        return false;
    }

    std::string_view payload(bytes);
    if (payload.size() >= kUtf8Bom.size() &&
        static_cast<unsigned char>(payload[0]) == kUtf8Bom[0] &&
        static_cast<unsigned char>(payload[1]) == kUtf8Bom[1] &&
        static_cast<unsigned char>(payload[2]) == kUtf8Bom[2])
    {
        payload.remove_prefix(kUtf8Bom.size());
    }

    if (!Utf8ToWide(payload, content))
    {
        ::SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return false;
    }

    return true;
}

bool WriteUtf8TextFile(const std::wstring& path, std::wstring_view content)
{
    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const std::string utf8_content = WideToUtf8(content);
    if (!content.empty() && utf8_content.empty())
    {
        ::CloseHandle(handle);
        ::SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return false;
    }

    bool write_succeeded = WriteAll(handle, kUtf8Bom.data(), static_cast<DWORD>(kUtf8Bom.size()));
    if (write_succeeded && !utf8_content.empty())
    {
        write_succeeded = WriteAll(handle, utf8_content.data(), static_cast<DWORD>(utf8_content.size()));
    }

    const DWORD error_code = write_succeeded ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);

    if (!write_succeeded)
    {
        ::SetLastError(error_code);
    }

    return write_succeeded;
}

bool TryParseUnsignedPart(std::wstring_view value, WORD& parsed_value)
{
    if (value.empty())
    {
        return false;
    }

    unsigned int accumulator = 0;
    for (const wchar_t character : value)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }

        accumulator = (accumulator * 10U) + static_cast<unsigned int>(character - L'0');
        if (accumulator > std::numeric_limits<WORD>::max())
        {
            return false;
        }
    }

    parsed_value = static_cast<WORD>(accumulator);
    return true;
}

bool TryConvertTimestampToFileTime(const Timestamp& timestamp, FILETIME& file_time)
{
    using namespace std::chrono;

    const auto epoch_nanoseconds = duration_cast<nanoseconds>(timestamp.time_since_epoch()).count();
    if (epoch_nanoseconds < 0)
    {
        return false;
    }

    const ULONGLONG ticks_since_unix_epoch = static_cast<ULONGLONG>(epoch_nanoseconds / 100);
    ULARGE_INTEGER value = {};
    value.QuadPart = kWindowsToUnixEpochTicks + ticks_since_unix_epoch;

    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    return true;
}

bool TryConvertFileTimeToTimestamp(const FILETIME& file_time, Timestamp& timestamp)
{
    using namespace std::chrono;

    ULARGE_INTEGER value = {};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < kWindowsToUnixEpochTicks)
    {
        return false;
    }

    const ULONGLONG ticks_since_unix_epoch = value.QuadPart - kWindowsToUnixEpochTicks;
    const auto epoch_duration = duration_cast<Timestamp::duration>(nanoseconds(ticks_since_unix_epoch * 100ULL));
    timestamp = Timestamp(epoch_duration);
    return true;
}

std::wstring FormatTimestampUtc(const Timestamp& timestamp)
{
    FILETIME file_time = {};
    if (!TryConvertTimestampToFileTime(timestamp, file_time))
    {
        return std::wstring();
    }

    SYSTEMTIME system_time = {};
    if (::FileTimeToSystemTime(&file_time, &system_time) == FALSE)
    {
        return std::wstring();
    }

    wchar_t buffer[32] = {};
    swprintf_s(
        buffer,
        L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        system_time.wYear,
        system_time.wMonth,
        system_time.wDay,
        system_time.wHour,
        system_time.wMinute,
        system_time.wSecond,
        system_time.wMilliseconds);

    return buffer;
}

bool TryParseTimestampUtc(std::wstring_view value, Timestamp& timestamp)
{
    if (value.size() != 24 ||
        value[4] != L'-' ||
        value[7] != L'-' ||
        value[10] != L'T' ||
        value[13] != L':' ||
        value[16] != L':' ||
        value[19] != L'.' ||
        value[23] != L'Z')
    {
        return false;
    }

    WORD year = 0;
    WORD month = 0;
    WORD day = 0;
    WORD hour = 0;
    WORD minute = 0;
    WORD second = 0;
    WORD millisecond = 0;

    if (!TryParseUnsignedPart(value.substr(0, 4), year) ||
        !TryParseUnsignedPart(value.substr(5, 2), month) ||
        !TryParseUnsignedPart(value.substr(8, 2), day) ||
        !TryParseUnsignedPart(value.substr(11, 2), hour) ||
        !TryParseUnsignedPart(value.substr(14, 2), minute) ||
        !TryParseUnsignedPart(value.substr(17, 2), second) ||
        !TryParseUnsignedPart(value.substr(20, 3), millisecond))
    {
        return false;
    }

    SYSTEMTIME system_time = {};
    system_time.wYear = year;
    system_time.wMonth = month;
    system_time.wDay = day;
    system_time.wHour = hour;
    system_time.wMinute = minute;
    system_time.wSecond = second;
    system_time.wMilliseconds = millisecond;

    FILETIME file_time = {};
    if (::SystemTimeToFileTime(&system_time, &file_time) == FALSE)
    {
        return false;
    }

    return TryConvertFileTimeToTimestamp(file_time, timestamp);
}

bool BuildConfigText(const AppConfig& config, std::wstring& text)
{
    AppConfig normalized = config;
    NormalizeConfig(normalized);

    std::wstring last_successful_move_value;
    if (normalized.last_successful_move_time.has_value())
    {
        last_successful_move_value = FormatTimestampUtc(*normalized.last_successful_move_time);
        if (last_successful_move_value.empty())
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return false;
        }
    }

    text.clear();
    text.reserve(
        256 + normalized.target_monitor_id.size() + normalized.log_level.size() +
        wcslen(ToConfigValue(normalized.tray_menu_language)));
    text += L"[";
    text += kConfigSectionGeneral;
    text += L"]\r\n";
    text += kConfigKeyTargetMonitorId;
    text += L"=";
    text += normalized.target_monitor_id;
    text += L"\r\n";
    text += kConfigKeyLastSuccessfulMoveUtc;
    text += L"=";
    text += last_successful_move_value;
    text += L"\r\n";
    text += kConfigKeyTrayMenuLanguage;
    text += L"=";
    text += ToConfigValue(normalized.tray_menu_language);
    text += L"\r\n";
    text += kConfigKeyLogLevel;
    text += L"=";
    text += normalized.log_level;
    text += L"\r\n";
    return true;
}

bool TryParseConfigText(std::wstring_view text, AppConfig& config, std::wstring& issues)
{
    AppConfig parsed = BuildDefaultConfig();
    std::wstring_view current_section;
    bool found_general_section = false;
    size_t line_number = 0;

    for (size_t line_start = 0; line_start <= text.size();)
    {
        ++line_number;

        size_t line_end = text.find(L'\n', line_start);
        if (line_end == std::wstring_view::npos)
        {
            line_end = text.size();
        }

        std::wstring_view line = text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == L'\r')
        {
            line.remove_suffix(1);
        }

        const std::wstring_view trimmed = Trim(line);
        if (!trimmed.empty() && trimmed.front() != L';' && trimmed.front() != L'#')
        {
            if (trimmed.front() == L'[')
            {
                if (trimmed.size() < 2 || trimmed.back() != L']')
                {
                    std::wstring issue = L"Malformed section header on line ";
                    issue += std::to_wstring(line_number);
                    issue += L".";
                    AppendIssue(issues, issue);
                }
                else
                {
                    current_section = Trim(trimmed.substr(1, trimmed.size() - 2));
                    found_general_section = found_general_section || current_section == std::wstring_view(kConfigSectionGeneral);
                }
            }
            else
            {
                const size_t delimiter_index = trimmed.find(L'=');
                if (delimiter_index == std::wstring_view::npos)
                {
                    std::wstring issue = L"Malformed key-value pair on line ";
                    issue += std::to_wstring(line_number);
                    issue += L".";
                    AppendIssue(issues, issue);
                }
                else if (current_section == std::wstring_view(kConfigSectionGeneral))
                {
                    const std::wstring_view key = Trim(trimmed.substr(0, delimiter_index));
                    const std::wstring_view value = Trim(trimmed.substr(delimiter_index + 1));

                    if (key.empty())
                    {
                        std::wstring issue = L"Empty key on line ";
                        issue += std::to_wstring(line_number);
                        issue += L".";
                        AppendIssue(issues, issue);
                    }
                    else if (key == std::wstring_view(kConfigKeyTargetMonitorId))
                    {
                        parsed.target_monitor_id.assign(value.data(), value.size());
                    }
                    else if (key == std::wstring_view(kConfigKeyLastSuccessfulMoveUtc))
                    {
                        if (value.empty())
                        {
                            parsed.last_successful_move_time.reset();
                        }
                        else
                        {
                            Timestamp parsed_time;
                            if (TryParseTimestampUtc(value, parsed_time))
                            {
                                parsed.last_successful_move_time = parsed_time;
                            }
                            else
                            {
                                parsed.last_successful_move_time.reset();

                                std::wstring issue = L"Invalid ";
                                issue += kConfigKeyLastSuccessfulMoveUtc;
                                issue += L" value on line ";
                                issue += std::to_wstring(line_number);
                                issue += L".";
                                AppendIssue(issues, issue);
                            }
                        }
                    }
                    else if (key == std::wstring_view(kConfigKeyTrayMenuLanguage))
                    {
                        if (value.empty())
                        {
                            parsed.tray_menu_language = TrayMenuLanguage::English;

                            std::wstring issue = L"Empty ";
                            issue += kConfigKeyTrayMenuLanguage;
                            issue += L" value on line ";
                            issue += std::to_wstring(line_number);
                            issue += L".";
                            AppendIssue(issues, issue);
                        }
                        else
                        {
                            TrayMenuLanguage parsed_language = TrayMenuLanguage::English;
                            if (TryParseTrayMenuLanguage(value, parsed_language))
                            {
                                parsed.tray_menu_language = parsed_language;
                            }
                            else
                            {
                                parsed.tray_menu_language = TrayMenuLanguage::English;

                                std::wstring issue = L"Invalid ";
                                issue += kConfigKeyTrayMenuLanguage;
                                issue += L" value on line ";
                                issue += std::to_wstring(line_number);
                                issue += L".";
                                AppendIssue(issues, issue);
                            }
                        }
                    }
                    else if (key == std::wstring_view(kConfigKeyLogLevel))
                    {
                        if (value.empty())
                        {
                            parsed.log_level = L"info";

                            std::wstring issue = L"Empty ";
                            issue += kConfigKeyLogLevel;
                            issue += L" value on line ";
                            issue += std::to_wstring(line_number);
                            issue += L".";
                            AppendIssue(issues, issue);
                        }
                        else
                        {
                            parsed.log_level.assign(value.data(), value.size());
                        }
                    }
                }
            }
        }

        if (line_end == text.size())
        {
            break;
        }

        line_start = line_end + 1;
    }

    if (!HasMeaningfulContent(text))
    {
        AppendIssue(issues, L"Config file is empty.");
    }
    else if (!found_general_section)
    {
        AppendIssue(issues, L"Missing [general] section.");
    }

    NormalizeConfig(parsed);
    config = std::move(parsed);
    return issues.empty();
}

bool IsRecoverableConfigReadError(DWORD error_code)
{
    switch (error_code)
    {
    case ERROR_FILE_TOO_LARGE:
    case ERROR_NO_UNICODE_TRANSLATION:
        return true;

    default:
        return false;
    }
}

bool SaveInternal(const AppConfig& config, const std::wstring& config_path)
{
    std::wstring text;
    if (!BuildConfigText(config, text))
    {
        return false;
    }

    const std::wstring temp_path = config_path + L".tmp";
    if (!WriteUtf8TextFile(temp_path, text))
    {
        return false;
    }

    if (::MoveFileExW(temp_path.c_str(), config_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        const DWORD error_code = ::GetLastError();
        ::DeleteFileW(temp_path.c_str());
        ::SetLastError(error_code);
        return false;
    }

    return true;
}

void LogRecoveredConfigWriteResult(const std::wstring& config_path, std::wstring_view recovery_reason)
{
    if (!SaveInternal(BuildDefaultConfig(), config_path))
    {
        const DWORD save_error = ::GetLastError();
        Logger::Error(
            kConfigPhase,
            std::wstring(recovery_reason) + L" Failed to persist recovered defaults. path=\"" + config_path + L"\" " +
                DescribeWin32Error(save_error));
        return;
    }

    Logger::Info(
        kConfigPhase,
        std::wstring(recovery_reason) + L" Recovered config file persisted. path=\"" + config_path + L"\"");
}
}  // namespace

std::wstring ConfigManager::GetConfigFilePath()
{
    const std::wstring directory_path = BuildConfigDirectoryPath();
    if (directory_path.empty())
    {
        return std::wstring();
    }

    return directory_path + L'\\' + kConfigFileName;
}

bool ConfigManager::Load(AppConfig& config)
{
    config = BuildDefaultConfig();

    const std::wstring directory_path = BuildConfigDirectoryPath();
    if (directory_path.empty())
    {
        Logger::Error(kConfigPhase, L"Failed to resolve the configuration directory from %APPDATA%; using defaults.");
        return false;
    }

    if (!EnsureDirectoryExists(directory_path))
    {
        const DWORD error_code = ::GetLastError();
        Logger::Error(
            kConfigPhase,
            L"Failed to create the configuration directory; using defaults. path=\"" + directory_path +
                L"\" " + DescribeWin32Error(error_code));
        return false;
    }

    const std::wstring config_path = directory_path + L'\\' + kConfigFileName;
    const DWORD attributes = ::GetFileAttributesW(config_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error_code = ::GetLastError();
        if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND)
        {
            Logger::Info(kConfigPhase, L"Config file not found; creating defaults. path=\"" + config_path + L"\"");

            if (!SaveInternal(config, config_path))
            {
                const DWORD save_error = ::GetLastError();
                Logger::Error(
                    kConfigPhase,
                    L"Failed to create the default config file; using in-memory defaults. path=\"" + config_path +
                        L"\" " + DescribeWin32Error(save_error));
                return false;
            }

            Logger::Info(kConfigPhase, L"Default config file created. path=\"" + config_path + L"\"");
            return true;
        }

        Logger::Error(
            kConfigPhase,
            L"Failed to inspect the config file; using defaults. path=\"" + config_path + L"\" " +
                DescribeWin32Error(error_code));
        return false;
    }

    std::wstring text;
    if (!ReadUtf8TextFile(config_path, text))
    {
        const DWORD error_code = ::GetLastError();
        Logger::Error(
            kConfigPhase,
            L"Failed to read the config file; using defaults. path=\"" + config_path + L"\" " +
                DescribeWin32Error(error_code));

        if (IsRecoverableConfigReadError(error_code))
        {
            LogRecoveredConfigWriteResult(config_path, L"Unreadable config content was detected.");
        }

        return false;
    }

    std::wstring issues;
    AppConfig parsed_config = BuildDefaultConfig();
    const bool parsed_cleanly = TryParseConfigText(text, parsed_config, issues);

    NormalizeConfig(parsed_config);
    config = std::move(parsed_config);

    if (!parsed_cleanly)
    {
        Logger::Warning(
            kConfigPhase,
            L"Config file contained invalid data; defaults were applied where needed. path=\"" + config_path +
                L"\" details=\"" + issues + L"\"");

        if (!SaveInternal(config, config_path))
        {
            const DWORD save_error = ::GetLastError();
            Logger::Error(
                kConfigPhase,
                L"Failed to persist the recovered config file. path=\"" + config_path + L"\" " +
                    DescribeWin32Error(save_error));
        }
        else
        {
            Logger::Info(kConfigPhase, L"Recovered config file persisted. path=\"" + config_path + L"\"");
        }

        return false;
    }

    Logger::Info(
        kConfigPhase,
        L"Configuration loaded. path=\"" + config_path + L"\" target_monitor_id=\"" + config.target_monitor_id +
            L"\" tray_menu_language=\"" + ToConfigValue(config.tray_menu_language) + L"\"");
    return true;
}

bool ConfigManager::Save(const AppConfig& config)
{
    const std::wstring directory_path = BuildConfigDirectoryPath();
    if (directory_path.empty())
    {
        Logger::Error(kConfigPhase, L"Failed to resolve the configuration directory from %APPDATA%; config changes were not saved.");
        return false;
    }

    if (!EnsureDirectoryExists(directory_path))
    {
        const DWORD error_code = ::GetLastError();
        Logger::Error(
            kConfigPhase,
            L"Failed to create the configuration directory; config changes were not saved. path=\"" + directory_path +
                L"\" " + DescribeWin32Error(error_code));
        return false;
    }

    const std::wstring config_path = directory_path + L'\\' + kConfigFileName;
    if (!SaveInternal(config, config_path))
    {
        const DWORD error_code = ::GetLastError();
        Logger::Error(
            kConfigPhase,
            L"Failed to save the config file. path=\"" + config_path + L"\" " + DescribeWin32Error(error_code));
        return false;
    }

    Logger::Info(kConfigPhase, L"Configuration saved. path=\"" + config_path + L"\"");
    return true;
}

bool ConfigManager::SetTargetMonitorId(AppConfig& config, std::wstring_view target_monitor_id)
{
    NormalizeConfig(config);

    const AppConfig previous_config = config;
    config.target_monitor_id.assign(target_monitor_id.data(), target_monitor_id.size());

    if (Save(config))
    {
        return true;
    }

    config = previous_config;
    return false;
}

bool ConfigManager::SetTrayMenuLanguage(AppConfig& config, TrayMenuLanguage tray_menu_language)
{
    NormalizeConfig(config);

    if (config.tray_menu_language == tray_menu_language)
    {
        return true;
    }

    const AppConfig previous_config = config;
    config.tray_menu_language = tray_menu_language;

    if (Save(config))
    {
        return true;
    }

    config = previous_config;
    return false;
}

bool ConfigManager::SetLastSuccessfulMoveTime(AppConfig& config, std::optional<Timestamp> timestamp)
{
    NormalizeConfig(config);

    const AppConfig previous_config = config;
    config.last_successful_move_time = timestamp;

    if (Save(config))
    {
        return true;
    }

    config = previous_config;
    return false;
}
}  // namespace icon_mover
