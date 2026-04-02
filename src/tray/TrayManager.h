#pragma once

#include <optional>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "core/Models.h"

namespace icon_mover
{
enum class TrayCallbackAction
{
    None,
    ShowContextMenu
};

enum class TrayCommandType
{
    MoveIconsNow,
    ToggleAutoStart,
    SelectTargetMonitor,
    ExitApplication
};

struct TrayCommand final
{
    TrayCommandType type = TrayCommandType::MoveIconsNow;
    std::wstring target_monitor_id;
};

struct TrayMenuContext final
{
    const MonitorTopology* monitor_topology = nullptr;
    const TargetMonitorSelection* target_monitor = nullptr;
    bool auto_start_enabled = false;
    bool auto_start_status_available = false;
};

struct TrayMenuMonitorCommand final
{
    UINT command_id = 0;
    std::wstring monitor_id;
};

class TrayManager final
{
public:
    TrayManager() noexcept = default;
    ~TrayManager();

    TrayManager(const TrayManager&) = delete;
    TrayManager& operator=(const TrayManager&) = delete;
    TrayManager(TrayManager&&) = delete;
    TrayManager& operator=(TrayManager&&) = delete;

    bool Initialize(HINSTANCE instance, HWND owner_window);
    void Shutdown() noexcept;

    [[nodiscard]] bool IsTaskbarCreatedMessage(UINT message) const noexcept;
    [[nodiscard]] bool HandleTaskbarCreated();
    [[nodiscard]] TrayCallbackAction HandleCallbackMessage(LPARAM l_param) const noexcept;
    [[nodiscard]] std::optional<TrayCommand> HandleCommand(WPARAM w_param) const;
    bool ShowContextMenu(const TrayMenuContext& context);

private:
    bool EnsureOwnerWindow(HWND owner_window);
    bool LoadTrayIcon();
    bool AddTrayIcon();
    bool SetTrayIconVersion();
    bool DeleteTrayIcon() noexcept;
    void FillNotifyIconData(NOTIFYICONDATAW& notify_icon_data) const noexcept;
    void ReleaseTrayIcon() noexcept;
    static DWORD ResolveNotifyIconError() noexcept;

    HINSTANCE instance_ = nullptr;
    HWND owner_window_ = nullptr;
    HICON tray_icon_ = nullptr;
    UINT taskbar_created_message_ = 0;
    bool tray_icon_added_ = false;
    bool owns_tray_icon_ = false;
    std::vector<TrayMenuMonitorCommand> dynamic_monitor_commands_;
};
}  // namespace icon_mover
