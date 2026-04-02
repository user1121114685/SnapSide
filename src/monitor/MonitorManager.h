#pragma once

#include <string_view>

#include <windows.h>

#include "core/Models.h"

namespace icon_mover
{
class MonitorManager final
{
public:
    static bool RefreshTopology(MonitorTopology& topology);
    static TargetMonitorSelection ResolveTargetMonitor(const MonitorTopology& topology, std::wstring_view requested_monitor_id);

    static const MonitorInfo* FindMonitorById(const MonitorTopology& topology, std::wstring_view monitor_id) noexcept;

private:
    MonitorManager() = delete;
};
}  // namespace icon_mover
