#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include "app/MessageWindow.h"
#include "core/AppState.h"
#include "desktop/DesktopViewAccessor.h"
#include "explorer/ExplorerWatcher.h"
#include "tray/TrayManager.h"

namespace icon_mover
{
struct LaunchOptions final
{
    bool log_test_error = false;
    std::optional<std::wstring> set_target_monitor_id;
};

class App final : private MessageWindowHost
{
public:
    App(HINSTANCE instance, LaunchOptions options) noexcept;

    int Run();

private:
    bool Initialize();
    bool LoadConfiguration();
    bool ApplyStartupConfigOverrides();
    bool CreateMessageWindow();
    int MessageLoop();
    bool ArmTimer(TimerId timer_id, UINT interval_ms);
    void DisarmTimer(TimerId timer_id) noexcept;
    void DisarmAllTimers() noexcept;
    bool ExecuteDesktopIconMove(MoveTrigger trigger);
    void HandleTimer(UINT_PTR timer_id);
    void HandleDisplayChange(UINT bits_per_pixel, int width, int height);
    bool RefreshDesktopViewContext();
    void SyncDesktopViewAccessor(ExplorerAvailability previous_availability, HWND previous_list_view_window);
    void BeginShutdown(int exit_code, std::wstring_view reason);

    std::optional<LRESULT> OnMessageWindowMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) override;

    HINSTANCE instance_;
    LaunchOptions launch_options_;
    AppState state_;
    MessageWindow message_window_;
    ExplorerWatcher explorer_watcher_;
    DesktopViewAccessor desktop_view_accessor_;
    TrayManager tray_manager_;
    int exit_code_;
    bool shutdown_requested_;
};
}  // namespace icon_mover

