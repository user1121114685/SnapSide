#pragma once

#include <windows.h>

#include "core/Models.h"

namespace icon_mover
{
enum class MonitorWorkAreaSource
{
    MonitorInfo,
    MonitorInfoClamped,
    MonitorInfoAndSystemIntersection,
    SystemParametersInfo,
    MonitorBoundsFallback
};

struct MonitorWorkAreaResult final
{
    RECT resolved_rect = {};
    MonitorWorkAreaSource source = MonitorWorkAreaSource::MonitorBoundsFallback;
    bool system_work_area_checked = false;
    bool system_work_area_compatible = false;
};

bool RefreshSystemWorkArea(VirtualScreenInfo& virtual_screen);
MonitorWorkAreaResult ResolveMonitorWorkArea(const MonitorInfo& monitor, const VirtualScreenInfo& virtual_screen) noexcept;
const wchar_t* ToString(MonitorWorkAreaSource source) noexcept;
}  // namespace icon_mover
