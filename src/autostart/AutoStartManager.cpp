#include "autostart/AutoStartManager.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include "IconMover/Version.h"
#include "common/Logger.h"
#include "common/Win32Error.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kAutoStartPhase[] = L"autostart";
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

struct ScopedRegistryKey final
{
    ScopedRegistryKey() noexcept = default;

    explicit ScopedRegistryKey(HKEY handle) noexcept
        : handle(handle)
    {
    }

    ~ScopedRegistryKey()
    {
        if (handle != nullptr)
        {
            ::RegCloseKey(handle);
            handle = nullptr;
        }
    }

    ScopedRegistryKey(const ScopedRegistryKey&) = delete;
    ScopedRegistryKey& operator=(const ScopedRegistryKey&) = delete;

    ScopedRegistryKey(ScopedRegistryKey&& other) noexcept
        : handle(other.handle)
    {
        other.handle = nullptr;
    }

    ScopedRegistryKey& operator=(ScopedRegistryKey&& other) noexcept
    {
        if (this != &other)
        {
            if (handle != nullptr)
            {
                ::RegCloseKey(handle);
            }

            handle = other.handle;
            other.handle = nullptr;
        }

        return *this;
    }

    HKEY handle = nullptr;
};

struct RunValueSnapshot final
{
    bool exists = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
    std::wstring string_value;
};

bool IsStringValueType(DWORD type) noexcept
{
    return type == REG_SZ || type == REG_EXPAND_SZ;
}

std::wstring BoolToString(bool value)
{
    return value ? L"true" : L"false";
}

bool EqualsCaseInsensitive(std::wstring_view left, std::wstring_view right)
{
    return ::CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

std::wstring DecodeWideStringValue(const std::vector<BYTE>& data)
{
    if (data.empty() || (data.size() % sizeof(wchar_t)) != 0)
    {
        return std::wstring();
    }

    const auto* characters = reinterpret_cast<const wchar_t*>(data.data());
    std::wstring value(characters, data.size() / sizeof(wchar_t));

    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }

    return value;
}

std::wstring DescribeRunValueSnapshot(const RunValueSnapshot& snapshot)
{
    std::wstring description = L"value_present=";
    description += BoolToString(snapshot.exists);

    if (!snapshot.exists)
    {
        return description;
    }

    description += L" value_type=";
    description += std::to_wstring(snapshot.type);
    description += L" value_size_bytes=";
    description += std::to_wstring(snapshot.data.size());

    if (IsStringValueType(snapshot.type))
    {
        description += L" value=\"";
        description += snapshot.string_value;
        description += L"\"";
    }

    return description;
}

std::wstring BuildRequestedStateLabel(bool enabled)
{
    return enabled ? L"enabled" : L"disabled";
}

DWORD OpenRunKey(REGSAM access_mask, ScopedRegistryKey& key)
{
    HKEY handle = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, access_mask, &handle);
    if (status != ERROR_SUCCESS)
    {
        return static_cast<DWORD>(status);
    }

    key = ScopedRegistryKey(handle);
    return ERROR_SUCCESS;
}

DWORD QueryRunValue(HKEY key, RunValueSnapshot& snapshot)
{
    snapshot = RunValueSnapshot{};

    DWORD type = REG_NONE;
    DWORD byte_count = 0;
    LSTATUS status = ::RegQueryValueExW(key, kApplicationName, nullptr, &type, nullptr, &byte_count);

    if (status == ERROR_FILE_NOT_FOUND)
    {
        return ERROR_SUCCESS;
    }

    if (status != ERROR_SUCCESS)
    {
        return static_cast<DWORD>(status);
    }

    snapshot.exists = true;
    snapshot.type = type;

    DWORD actual_size = byte_count;
    snapshot.data.resize(actual_size);

    while (true)
    {
        actual_size = static_cast<DWORD>(snapshot.data.size());
        status = ::RegQueryValueExW(
            key,
            kApplicationName,
            nullptr,
            &type,
            snapshot.data.empty() ? nullptr : snapshot.data.data(),
            &actual_size);

        if (status != ERROR_MORE_DATA)
        {
            break;
        }

        snapshot.data.resize(actual_size);
    }

    if (status != ERROR_SUCCESS)
    {
        snapshot = RunValueSnapshot{};
        return static_cast<DWORD>(status);
    }

    snapshot.type = type;
    snapshot.data.resize(actual_size);

    if (IsStringValueType(type))
    {
        snapshot.string_value = DecodeWideStringValue(snapshot.data);
    }

    return ERROR_SUCCESS;
}

DWORD GetCurrentExecutablePath(std::wstring& path)
{
    path.clear();

    std::wstring buffer(static_cast<std::size_t>(MAX_PATH), L'\0');
    while (buffer.size() < 32768)
    {
        const DWORD copied = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0)
        {
            return ::GetLastError();
        }

        if (copied < buffer.size())
        {
            buffer.resize(copied);
            path = std::move(buffer);
            return ERROR_SUCCESS;
        }

        buffer.resize(buffer.size() * 2);
    }

    return ERROR_BUFFER_OVERFLOW;
}

DWORD BuildCurrentExecutableCommandLine(std::wstring& command_line)
{
    std::wstring executable_path;
    const DWORD path_error = GetCurrentExecutablePath(executable_path);
    if (path_error != ERROR_SUCCESS)
    {
        return path_error;
    }

    command_line.clear();
    command_line.reserve(executable_path.size() + 2);
    command_line += L'"';
    command_line += executable_path;
    command_line += L'"';
    return ERROR_SUCCESS;
}

DWORD WriteRunValue(HKEY key, std::wstring_view command_line)
{
    const DWORD byte_count = static_cast<DWORD>((command_line.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = ::RegSetValueExW(
        key,
        kApplicationName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command_line.data()),
        byte_count);

    return status == ERROR_SUCCESS ? ERROR_SUCCESS : static_cast<DWORD>(status);
}

DWORD DeleteRunValue(HKEY key)
{
    const LSTATUS status = ::RegDeleteValueW(key, kApplicationName);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
    {
        return ERROR_SUCCESS;
    }

    return static_cast<DWORD>(status);
}

DWORD RestoreRunValue(HKEY key, const RunValueSnapshot& snapshot)
{
    if (!snapshot.exists)
    {
        return DeleteRunValue(key);
    }

    const LSTATUS status = ::RegSetValueExW(
        key,
        kApplicationName,
        0,
        snapshot.type,
        snapshot.data.empty() ? nullptr : snapshot.data.data(),
        static_cast<DWORD>(snapshot.data.size()));

    return status == ERROR_SUCCESS ? ERROR_SUCCESS : static_cast<DWORD>(status);
}

bool PopulateStatusFromSnapshot(
    const RunValueSnapshot& snapshot,
    std::wstring_view current_command_line,
    AutoStartStatus& status)
{
    status.enabled = snapshot.exists;
    status.command_line = snapshot.string_value;
    status.matches_current_executable =
        snapshot.exists && IsStringValueType(snapshot.type) && EqualsCaseInsensitive(snapshot.string_value, current_command_line);
    return true;
}

bool VerifySnapshotMatchesRequestedState(
    const RunValueSnapshot& snapshot,
    bool enabled,
    std::wstring_view expected_command_line) noexcept
{
    if (!enabled)
    {
        return !snapshot.exists;
    }

    return snapshot.exists && IsStringValueType(snapshot.type) && EqualsCaseInsensitive(snapshot.string_value, expected_command_line);
}
}  // namespace

bool AutoStartManager::Query(AutoStartStatus& status)
{
    status = AutoStartStatus{};

    ScopedRegistryKey key;
    const DWORD open_error = OpenRunKey(KEY_QUERY_VALUE, key);
    if (open_error == ERROR_FILE_NOT_FOUND)
    {
        return true;
    }

    if (open_error != ERROR_SUCCESS)
    {
        Logger::Error(
            kAutoStartPhase,
            L"Failed to open the HKCU Run registry key for auto-start query. " + DescribeWin32Error(open_error));
        return false;
    }

    RunValueSnapshot snapshot;
    const DWORD query_error = QueryRunValue(key.handle, snapshot);
    if (query_error != ERROR_SUCCESS)
    {
        Logger::Error(
            kAutoStartPhase,
            L"Failed to query the auto-start registry value. " + DescribeWin32Error(query_error));
        return false;
    }

    std::wstring current_command_line;
    const DWORD command_error = BuildCurrentExecutableCommandLine(current_command_line);
    if (command_error != ERROR_SUCCESS)
    {
        current_command_line.clear();
    }

    return PopulateStatusFromSnapshot(snapshot, current_command_line, status);
}

bool AutoStartManager::SetEnabled(bool enabled)
{
    const std::wstring requested_state = BuildRequestedStateLabel(enabled);

    ScopedRegistryKey key;
    const DWORD open_error = OpenRunKey(KEY_QUERY_VALUE | KEY_SET_VALUE, key);
    if (open_error == ERROR_FILE_NOT_FOUND && !enabled)
    {
        Logger::Info(kAutoStartPhase, L"Auto-start already disabled because the HKCU Run registry key is unavailable.");
        return true;
    }

    if (open_error != ERROR_SUCCESS)
    {
        Logger::Error(
            kAutoStartPhase,
            L"Failed to open the HKCU Run registry key for auto-start state change. requested_state=\"" +
                requested_state + L"\" " + DescribeWin32Error(open_error));
        return false;
    }

    RunValueSnapshot previous_snapshot;
    const DWORD previous_query_error = QueryRunValue(key.handle, previous_snapshot);
    if (previous_query_error != ERROR_SUCCESS)
    {
        Logger::Error(
            kAutoStartPhase,
            L"Failed to capture the current auto-start registry value before updating it. requested_state=\"" +
                requested_state + L"\" " + DescribeWin32Error(previous_query_error));
        return false;
    }

    std::wstring expected_command_line;
    if (enabled)
    {
        const DWORD command_error = BuildCurrentExecutableCommandLine(expected_command_line);
        if (command_error != ERROR_SUCCESS)
        {
            Logger::Error(
                kAutoStartPhase,
                L"Failed to resolve the current executable path for auto-start enable. " +
                    DescribeWin32Error(command_error));
            return false;
        }

        if (VerifySnapshotMatchesRequestedState(previous_snapshot, true, expected_command_line))
        {
            Logger::Info(
                kAutoStartPhase,
                L"Auto-start is already enabled with the current executable path. " +
                    DescribeRunValueSnapshot(previous_snapshot));
            return true;
        }
    }
    else if (!previous_snapshot.exists)
    {
        Logger::Info(kAutoStartPhase, L"Auto-start is already disabled.");
        return true;
    }

    DWORD mutation_error = ERROR_SUCCESS;
    if (enabled)
    {
        mutation_error = WriteRunValue(key.handle, expected_command_line);
    }
    else
    {
        mutation_error = DeleteRunValue(key.handle);
    }

    if (mutation_error != ERROR_SUCCESS)
    {
        Logger::Error(
            kAutoStartPhase,
            L"Failed to update the auto-start registry value. requested_state=\"" + requested_state + L"\" " +
                DescribeWin32Error(mutation_error) + L" previous_state={" + DescribeRunValueSnapshot(previous_snapshot) + L"}");
        return false;
    }

    RunValueSnapshot actual_snapshot;
    const DWORD actual_query_error = QueryRunValue(key.handle, actual_snapshot);
    if (actual_query_error != ERROR_SUCCESS ||
        !VerifySnapshotMatchesRequestedState(actual_snapshot, enabled, expected_command_line))
    {
        const DWORD rollback_error = RestoreRunValue(key.handle, previous_snapshot);
        std::wstring message = L"Auto-start registry verification failed after updating the Run value. requested_state=\"";
        message += requested_state;
        message += L"\" ";

        if (actual_query_error != ERROR_SUCCESS)
        {
            message += DescribeWin32Error(actual_query_error);
        }
        else
        {
            message += L"actual_state={";
            message += DescribeRunValueSnapshot(actual_snapshot);
            message += L"}";
        }

        message += L" previous_state={";
        message += DescribeRunValueSnapshot(previous_snapshot);
        message += L"}";

        if (rollback_error == ERROR_SUCCESS)
        {
            Logger::Error(kAutoStartPhase, message + L" rollback_result=\"restored\"");
        }
        else
        {
            Logger::Error(
                kAutoStartPhase,
                message + L" rollback_result=\"failed\" " + DescribeWin32Error(rollback_error));
        }

        return false;
    }

    std::wstring success_message = L"Auto-start state updated successfully. requested_state=\"";
    success_message += requested_state;
    success_message += L"\" actual_state={";
    success_message += DescribeRunValueSnapshot(actual_snapshot);
    success_message += L"}";
    Logger::Info(kAutoStartPhase, success_message);
    return true;
}

bool AutoStartManager::Enable()
{
    return SetEnabled(true);
}

bool AutoStartManager::Disable()
{
    return SetEnabled(false);
}
}  // namespace icon_mover
