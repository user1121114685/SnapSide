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
    bool auto_start_launch = false;
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
    bool LoadAutoStartStatus();
    bool ApplyStartupConfigOverrides();
    bool CreateMessageWindow();
    void RegisterExtendDisplaysHotkey();
    void UnregisterExtendDisplaysHotkey();
    void RequestExtendedDisplayMode(std::wstring_view reason);
    bool StartExplorerReadinessWait();
    int MessageLoop();
    bool ArmTimer(TimerId timer_id, UINT interval_ms);
    void DisarmTimer(TimerId timer_id) noexcept;
    void DisarmAllTimers() noexcept;
    bool ExecuteDesktopIconMove(MoveTrigger trigger);
    bool RefreshMonitorStateForDisplayChange();
    bool RefreshExplorerState(std::wstring_view reason, bool allow_immediate_display_change_move);
    void StartDisplayChangeMoveSession();
    void ClearDisplayChangeMoveSession(std::wstring_view reason, bool warning);
    bool EnsureDisplayChangeMoveSessionAlive(std::wstring_view reason);
    bool ScheduleFirstAutoMove(UINT interval_ms, std::wstring_view reason);
    void HandleTimer(UINT_PTR timer_id);
    void HandleFirstAutoMoveTimer();
    void HandleDisplayChange(UINT bits_per_pixel, int width, int height);
    void HandleDisplayChangeDebounceTimer();
    void HandleExplorerStateUpdate(ExplorerAvailability previous_availability, HWND previous_list_view_window);
    void TryExecutePendingDisplayChangeMove(std::wstring_view reason);
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
    bool extend_displays_hotkey_registered_ = false;
    bool pending_display_change_move_;
    bool suppress_immediate_display_change_move_;
    bool display_change_move_session_active_;
    ULONGLONG display_change_generation_;
    ULONGLONG display_change_started_tick_;
    UINT display_change_move_attempt_count_;
};
}  // namespace icon_mover

