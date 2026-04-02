#pragma once

#include <optional>

#include "core/Constants.h"
#include "core/Models.h"

namespace icon_mover
{
struct ExplorerWindowState final
{
    ExplorerAvailability availability = ExplorerAvailability::Unknown;
    HWND progman_window = nullptr;
    HWND desktop_host_window = nullptr;
    HWND desktop_list_view_window = nullptr;
    DWORD discovery_attempts = 0;
};

struct TimerState final
{
    bool explorer_retry_armed = false;
    bool first_auto_move_armed = false;
    bool display_change_debounce_armed = false;
};

struct AppState final
{
    AppLifecycleState lifecycle = AppLifecycleState::Bootstrap;
    AppConfig config;
    MonitorTopology monitor_topology;
    TargetMonitorSelection target_monitor;
    ExplorerWindowState explorer;
    TimerState timers;
    bool auto_start_enabled = false;
    bool auto_start_status_available = false;
    FirstAutoMoveState first_auto_move_state = FirstAutoMoveState::Pending;
    std::optional<Timestamp> last_execution_time;
    std::optional<MoveResult> last_move_result;
};
}  // namespace icon_mover
