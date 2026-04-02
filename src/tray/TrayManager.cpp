#include "tray/TrayManager.h"

#include <cwchar>
#include <optional>
#include <string>

#include <shellapi.h>

#include "IconMover/Version.h"
#include "common/Logger.h"
#include "common/Win32Error.h"
#include "core/Constants.h"
#include "resource/resource.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kTrayPhase[] = L"tray";
constexpr wchar_t kTaskbarCreatedMessageName[] = L"TaskbarCreated";
constexpr UINT kTrayIconId = 1;
}  // namespace

TrayManager::~TrayManager()
{
    Shutdown();
}

bool TrayManager::Initialize(HINSTANCE instance, HWND owner_window)
{
    if (tray_icon_added_ && instance_ == instance && owner_window_ == owner_window)
    {
        return true;
    }

    Shutdown();

    if (instance == nullptr)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Cannot initialize the tray icon manager without a module instance.");
        return false;
    }

    instance_ = instance;
    if (!EnsureOwnerWindow(owner_window))
    {
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Cannot initialize the tray icon manager without a valid owner window.");
        Shutdown();
        return false;
    }

    taskbar_created_message_ = ::RegisterWindowMessageW(kTaskbarCreatedMessageName);
    if (taskbar_created_message_ == 0)
    {
        if (::GetLastError() == ERROR_SUCCESS)
        {
            ::SetLastError(ERROR_GEN_FAILURE);
        }

        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to register the Explorer taskbar recreation message.");
        Shutdown();
        return false;
    }

    if (!LoadTrayIcon())
    {
        Shutdown();
        return false;
    }

    if (!AddTrayIcon())
    {
        Logger::Error(kTrayPhase, L"Tray icon initialization failed because the initial Shell_NotifyIcon registration did not succeed.");
        Shutdown();
        return false;
    }

    Logger::Info(kTrayPhase, L"Tray icon initialized successfully.");
    return true;
}

void TrayManager::Shutdown() noexcept
{
    static_cast<void>(DeleteTrayIcon());
    ReleaseTrayIcon();
    taskbar_created_message_ = 0;
    owner_window_ = nullptr;
    instance_ = nullptr;
}

bool TrayManager::IsTaskbarCreatedMessage(UINT message) const noexcept
{
    return taskbar_created_message_ != 0 && message == taskbar_created_message_;
}

bool TrayManager::HandleTaskbarCreated()
{
    if (!EnsureOwnerWindow(owner_window_))
    {
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Cannot re-register the tray icon because the owner window is no longer valid.");
        return false;
    }

    Logger::Info(kTrayPhase, L"Explorer taskbar recreation detected; re-registering the tray icon.");
    tray_icon_added_ = false;
    return AddTrayIcon();
}

void TrayManager::HandleCallbackMessage(LPARAM l_param)
{
    if (!tray_icon_added_)
    {
        return;
    }

    const UINT notification_code = LOWORD(static_cast<DWORD_PTR>(l_param));
    switch (notification_code)
    {
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP:
    case NIN_KEYSELECT:
        static_cast<void>(ShowContextMenu());
        return;

    default:
        return;
    }
}

std::optional<TrayCommand> TrayManager::HandleCommand(WPARAM w_param) const
{
    switch (LOWORD(w_param))
    {
    case ID_TRAY_MOVE_ICONS_NOW:
        return TrayCommand::MoveIconsNow;

    case ID_TRAY_EXIT:
        return TrayCommand::ExitApplication;

    default:
        return std::nullopt;
    }
}

bool TrayManager::EnsureOwnerWindow(HWND owner_window)
{
    if (owner_window == nullptr || ::IsWindow(owner_window) == FALSE)
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }

    owner_window_ = owner_window;
    return true;
}

bool TrayManager::LoadTrayIcon()
{
    ReleaseTrayIcon();

    const int icon_width = ::GetSystemMetrics(SM_CXSMICON);
    const int icon_height = ::GetSystemMetrics(SM_CYSMICON);

    tray_icon_ = static_cast<HICON>(::LoadImageW(
        instance_,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        icon_width,
        icon_height,
        LR_DEFAULTCOLOR));

    if (tray_icon_ != nullptr)
    {
        owns_tray_icon_ = true;
        return true;
    }

    DWORD resource_error = ::GetLastError();
    if (resource_error == ERROR_SUCCESS)
    {
        resource_error = ERROR_RESOURCE_NAME_NOT_FOUND;
    }

    Logger::Warning(
        kTrayPhase,
        L"Failed to load the application tray icon resource; falling back to the default application icon. " +
            DescribeWin32Error(resource_error));

    tray_icon_ = ::LoadIconW(nullptr, IDI_APPLICATION);
    if (tray_icon_ == nullptr)
    {
        if (::GetLastError() == ERROR_SUCCESS)
        {
            ::SetLastError(ERROR_GEN_FAILURE);
        }

        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to load the fallback tray icon.");
        return false;
    }

    owns_tray_icon_ = false;
    return true;
}

bool TrayManager::AddTrayIcon()
{
    NOTIFYICONDATAW notify_icon_data = {};
    FillNotifyIconData(notify_icon_data);

    ::SetLastError(ERROR_SUCCESS);
    if (::Shell_NotifyIconW(NIM_ADD, &notify_icon_data) == FALSE)
    {
        const DWORD error_code = ResolveNotifyIconError();
        Logger::Error(kTrayPhase, L"Failed to add the tray icon. " + DescribeWin32Error(error_code));
        return false;
    }

    tray_icon_added_ = true;

    if (!SetTrayIconVersion())
    {
        Logger::Error(
            kTrayPhase,
            L"Tray icon registration succeeded, but upgrading to NOTIFYICON_VERSION_4 failed; removing the tray icon.");
        static_cast<void>(DeleteTrayIcon());
        return false;
    }

    Logger::Info(kTrayPhase, L"Tray icon registered with the Windows notification area.");
    return true;
}

bool TrayManager::SetTrayIconVersion()
{
    NOTIFYICONDATAW notify_icon_data = {};
    notify_icon_data.cbSize = sizeof(notify_icon_data);
    notify_icon_data.hWnd = owner_window_;
    notify_icon_data.uID = kTrayIconId;
    notify_icon_data.uVersion = NOTIFYICON_VERSION_4;

    ::SetLastError(ERROR_SUCCESS);
    if (::Shell_NotifyIconW(NIM_SETVERSION, &notify_icon_data) == FALSE)
    {
        const DWORD error_code = ResolveNotifyIconError();
        Logger::Error(kTrayPhase, L"Failed to set NOTIFYICON_VERSION_4 for the tray icon. " + DescribeWin32Error(error_code));
        return false;
    }

    return true;
}

bool TrayManager::DeleteTrayIcon() noexcept
{
    if (!tray_icon_added_)
    {
        return true;
    }

    NOTIFYICONDATAW notify_icon_data = {};
    notify_icon_data.cbSize = sizeof(notify_icon_data);
    notify_icon_data.hWnd = owner_window_;
    notify_icon_data.uID = kTrayIconId;

    ::SetLastError(ERROR_SUCCESS);
    const BOOL removed = ::Shell_NotifyIconW(NIM_DELETE, &notify_icon_data);
    const DWORD error_code = removed != FALSE ? ERROR_SUCCESS : ResolveNotifyIconError();
    tray_icon_added_ = false;

    if (removed == FALSE)
    {
        Logger::Warning(kTrayPhase, L"Failed to remove the tray icon. " + DescribeWin32Error(error_code));
        return false;
    }

    Logger::Info(kTrayPhase, L"Tray icon removed.");
    return true;
}

bool TrayManager::ShowContextMenu() const
{
    if (owner_window_ == nullptr || ::IsWindow(owner_window_) == FALSE)
    {
        return false;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
    {
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to create the tray context menu.");
        return false;
    }

    if (::AppendMenuW(menu, MF_STRING, ID_TRAY_MOVE_ICONS_NOW, L"Move Icons Now") == FALSE)
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(menu);
        ::SetLastError(error_code);
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to append the Move Icons Now tray menu item.");
        return false;
    }

    if (::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) == FALSE)
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(menu);
        ::SetLastError(error_code);
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to append the tray menu separator.");
        return false;
    }

    if (::AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit") == FALSE)
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(menu);
        ::SetLastError(error_code);
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to append the Exit tray menu item.");
        return false;
    }

    POINT cursor_position = {};
    if (::GetCursorPos(&cursor_position) == FALSE)
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(menu);
        ::SetLastError(error_code);
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to resolve the cursor position for the tray context menu.");
        return false;
    }

    ::SetForegroundWindow(owner_window_);
    ::SetLastError(ERROR_SUCCESS);
    const BOOL shown = ::TrackPopupMenuEx(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        cursor_position.x,
        cursor_position.y,
        owner_window_,
        nullptr);

    const DWORD error_code = shown != FALSE ? ERROR_SUCCESS : ::GetLastError();
    ::DestroyMenu(menu);
    ::PostMessageW(owner_window_, WM_NULL, 0, 0);

    if (shown == FALSE && error_code != ERROR_SUCCESS)
    {
        ::SetLastError(error_code);
        Logger::LogLastError(LogLevel::Error, kTrayPhase, L"Failed to show the tray context menu.");
        return false;
    }

    Logger::Info(kTrayPhase, L"Tray context menu requested.");
    return true;
}

void TrayManager::FillNotifyIconData(NOTIFYICONDATAW& notify_icon_data) const noexcept
{
    notify_icon_data = {};
    notify_icon_data.cbSize = sizeof(notify_icon_data);
    notify_icon_data.hWnd = owner_window_;
    notify_icon_data.uID = kTrayIconId;
    notify_icon_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    notify_icon_data.uCallbackMessage = kTrayIconMessage;
    notify_icon_data.hIcon = tray_icon_;
    wcscpy_s(notify_icon_data.szTip, kApplicationName);
}

void TrayManager::ReleaseTrayIcon() noexcept
{
    if (tray_icon_ != nullptr && owns_tray_icon_)
    {
        ::DestroyIcon(tray_icon_);
    }

    tray_icon_ = nullptr;
    owns_tray_icon_ = false;
}

DWORD TrayManager::ResolveNotifyIconError() noexcept
{
    const DWORD error_code = ::GetLastError();
    return error_code != ERROR_SUCCESS ? error_code : ERROR_GEN_FAILURE;
}
}  // namespace icon_mover
