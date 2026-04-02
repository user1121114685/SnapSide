#pragma once

#include <optional>

#include <windows.h>
#include <shellapi.h>

namespace icon_mover
{
enum class TrayCommand
{
    MoveIconsNow,
    ExitApplication
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
    void HandleCallbackMessage(LPARAM l_param);
    [[nodiscard]] std::optional<TrayCommand> HandleCommand(WPARAM w_param) const;

private:
    bool EnsureOwnerWindow(HWND owner_window);
    bool LoadTrayIcon();
    bool AddTrayIcon();
    bool SetTrayIconVersion();
    bool DeleteTrayIcon() noexcept;
    bool ShowContextMenu() const;
    void FillNotifyIconData(NOTIFYICONDATAW& notify_icon_data) const noexcept;
    void ReleaseTrayIcon() noexcept;
    static DWORD ResolveNotifyIconError() noexcept;

    HINSTANCE instance_ = nullptr;
    HWND owner_window_ = nullptr;
    HICON tray_icon_ = nullptr;
    UINT taskbar_created_message_ = 0;
    bool tray_icon_added_ = false;
    bool owns_tray_icon_ = false;
};
}  // namespace icon_mover
