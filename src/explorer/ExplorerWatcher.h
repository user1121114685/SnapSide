#pragma once

#include <windows.h>

#include "core/AppState.h"

namespace icon_mover
{
class ExplorerWatcher final
{
public:
    ExplorerWatcher() noexcept = default;

    ExplorerWatcher(const ExplorerWatcher&) = delete;
    ExplorerWatcher& operator=(const ExplorerWatcher&) = delete;

    bool Start(HWND message_window, ExplorerWindowState& explorer_state, TimerState& timer_state);
    void Stop(TimerState& timer_state) noexcept;
    bool HandleTimer(UINT_PTR timer_id, ExplorerWindowState& explorer_state, TimerState& timer_state);
    HWND GetDesktopListViewWindow(HWND message_window, ExplorerWindowState& explorer_state, TimerState& timer_state);

private:
    struct DiscoveryResult final
    {
        ExplorerAvailability availability = ExplorerAvailability::Unknown;
        HWND progman_window = nullptr;
        HWND desktop_host_window = nullptr;
        HWND desktop_list_view_window = nullptr;
    };

    static DiscoveryResult DiscoverExplorerWindows() noexcept;
    static bool IsWaitingAvailability(ExplorerAvailability availability) noexcept;

    bool EnsureOwnerWindow(HWND message_window);
    bool EnsureRetryTimer(TimerState& timer_state);
    void CancelRetryTimer(TimerState& timer_state) noexcept;
    bool HasValidReadyHandles(const ExplorerWindowState& explorer_state) const noexcept;
    void ResetWaitTracking() noexcept;
    void NoteHandleLoss(ExplorerWindowState& explorer_state) noexcept;
    HWND RefreshDesktopListViewWindow(ExplorerWindowState& explorer_state, TimerState& timer_state);
    void LogWaitingState(const ExplorerWindowState& explorer_state);
    void LogReadyState(const ExplorerWindowState& explorer_state) const;

    HWND owner_window_ = nullptr;
    ULONGLONG wait_started_tick_ = 0;
    ULONGLONG last_wait_log_tick_ = 0;
    ExplorerAvailability last_logged_wait_state_ = ExplorerAvailability::Unknown;
    bool background_wait_logged_ = false;
};
}  // namespace icon_mover
