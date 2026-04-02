#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace icon_mover
{
using Timestamp = std::chrono::system_clock::time_point;

struct MonitorInfo final
{
    HMONITOR handle = nullptr;
    UINT display_index = 0;
    std::wstring device_name;
    std::wstring monitor_id;
    bool is_primary = false;
    RECT monitor_rect = {};
    RECT work_area_rect = {};
};

struct VirtualScreenInfo final
{
    RECT bounds = {};
    RECT system_work_area = {};
    int monitor_count = 0;
    bool has_secondary_monitor = false;
};

struct MonitorTopology final
{
    VirtualScreenInfo virtual_screen;
    std::vector<MonitorInfo> monitors;
    std::optional<std::size_t> primary_monitor_index;
};

struct AppConfig final
{
    std::wstring target_monitor_id;
    std::optional<Timestamp> last_successful_move_time;
    std::wstring log_level = L"info";
};

enum class AppLifecycleState
{
    Bootstrap,
    Running,
    ShuttingDown
};

enum class ExplorerAvailability
{
    Unknown,
    WaitingForProgman,
    WaitingForDesktopHost,
    WaitingForListView,
    Ready,
    Lost
};

enum class FirstAutoMoveState
{
    Pending,
    WaitingForExplorer,
    ReadyToRun,
    Running,
    Completed,
    Failed
};

enum class MoveTrigger
{
    StartupAuto,
    ManualCommand,
    DisplayChange,
    TargetMonitorChanged,
    ExplorerRecovered
};

enum class MoveOutcome
{
    Success,
    Skipped,
    Failed
};

enum class MoveSkipReason
{
    None,
    ExplorerNotReady,
    NoAvailableMonitor,
    TargetMonitorUnavailable,
    NoDesktopIcons
};

enum class TargetMonitorResolution
{
    Unresolved,
    ConfiguredMonitor,
    FirstSecondaryMonitor,
    PrimaryMonitorFallback,
    Unavailable
};

struct TargetMonitorSelection final
{
    std::wstring requested_monitor_id;
    std::wstring resolved_monitor_id;
    TargetMonitorResolution resolution = TargetMonitorResolution::Unresolved;
};

struct MoveResult final
{
    MoveTrigger trigger = MoveTrigger::ManualCommand;
    MoveOutcome outcome = MoveOutcome::Skipped;
    MoveSkipReason skip_reason = MoveSkipReason::None;
    std::wstring target_monitor_id;
    std::optional<Timestamp> executed_at;
    int icon_count = 0;
    DWORD last_error = ERROR_SUCCESS;
};
}  // namespace icon_mover
