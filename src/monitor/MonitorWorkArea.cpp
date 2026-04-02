#include "monitor/MonitorWorkArea.h"

namespace icon_mover
{
namespace
{
bool IsRectValid(const RECT& rect) noexcept
{
    return rect.left < rect.right && rect.top < rect.bottom;
}

bool RectEquals(const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
}

RECT IntersectRects(const RECT& left, const RECT& right) noexcept
{
    RECT intersection = {};
    if (::IntersectRect(&intersection, &left, &right) == FALSE)
    {
        ::SetRectEmpty(&intersection);
    }

    return intersection;
}

RECT ResolveMonitorBounds(const MonitorInfo& monitor, const VirtualScreenInfo& virtual_screen) noexcept
{
    if (IsRectValid(monitor.monitor_rect))
    {
        const RECT bounded_monitor = IntersectRects(monitor.monitor_rect, virtual_screen.bounds);
        if (IsRectValid(bounded_monitor))
        {
            return bounded_monitor;
        }

        return monitor.monitor_rect;
    }

    return virtual_screen.bounds;
}

RECT ResolveMonitorInfoCandidate(
    const MonitorInfo& monitor,
    const VirtualScreenInfo& virtual_screen,
    MonitorWorkAreaSource& source) noexcept
{
    const RECT monitor_bounds = ResolveMonitorBounds(monitor, virtual_screen);
    if (!IsRectValid(monitor.raw_work_area_rect))
    {
        source = MonitorWorkAreaSource::MonitorBoundsFallback;
        return monitor_bounds;
    }

    RECT bounded_work_area = IntersectRects(monitor.raw_work_area_rect, monitor_bounds);
    if (!IsRectValid(bounded_work_area))
    {
        source = MonitorWorkAreaSource::MonitorBoundsFallback;
        return monitor_bounds;
    }

    bounded_work_area = IntersectRects(bounded_work_area, virtual_screen.bounds);
    if (!IsRectValid(bounded_work_area))
    {
        source = MonitorWorkAreaSource::MonitorBoundsFallback;
        return monitor_bounds;
    }

    source = RectEquals(bounded_work_area, monitor.raw_work_area_rect) ? MonitorWorkAreaSource::MonitorInfo
                                                                       : MonitorWorkAreaSource::MonitorInfoClamped;
    return bounded_work_area;
}
}  // namespace

bool RefreshSystemWorkArea(VirtualScreenInfo& virtual_screen)
{
    RECT system_work_area = {};
    virtual_screen.system_work_area = {};
    virtual_screen.has_system_work_area = false;

    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &system_work_area, 0) == FALSE)
    {
        return false;
    }

    if (!IsRectValid(system_work_area))
    {
        ::SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    virtual_screen.system_work_area = system_work_area;
    virtual_screen.has_system_work_area = true;
    return true;
}

MonitorWorkAreaResult ResolveMonitorWorkArea(const MonitorInfo& monitor, const VirtualScreenInfo& virtual_screen) noexcept
{
    MonitorWorkAreaResult result = {};

    MonitorWorkAreaSource monitor_info_source = MonitorWorkAreaSource::MonitorBoundsFallback;
    RECT resolved_rect = ResolveMonitorInfoCandidate(monitor, virtual_screen, monitor_info_source);
    result.resolved_rect = resolved_rect;
    result.source = monitor_info_source;

    if (!monitor.is_primary || !virtual_screen.has_system_work_area)
    {
        return result;
    }

    const RECT monitor_bounds = ResolveMonitorBounds(monitor, virtual_screen);
    const RECT system_work_area = IntersectRects(virtual_screen.system_work_area, monitor_bounds);
    if (!IsRectValid(system_work_area))
    {
        return result;
    }

    result.system_work_area_checked = true;

    if (!IsRectValid(resolved_rect))
    {
        result.resolved_rect = system_work_area;
        result.source = MonitorWorkAreaSource::SystemParametersInfo;
        result.system_work_area_compatible = false;
        return result;
    }

    // SPI_GETWORKAREA is a primary-monitor compatibility signal; intersect it with the
    // monitor-derived work area so icon layout never grows back into the taskbar region.
    const RECT merged_rect = IntersectRects(resolved_rect, system_work_area);
    if (!IsRectValid(merged_rect))
    {
        result.resolved_rect = system_work_area;
        result.source = MonitorWorkAreaSource::SystemParametersInfo;
        result.system_work_area_compatible = false;
        return result;
    }

    result.system_work_area_compatible = RectEquals(merged_rect, resolved_rect);
    if (!RectEquals(merged_rect, resolved_rect))
    {
        result.resolved_rect = merged_rect;
        result.source = MonitorWorkAreaSource::MonitorInfoAndSystemIntersection;
    }

    return result;
}

const wchar_t* ToString(MonitorWorkAreaSource source) noexcept
{
    switch (source)
    {
    case MonitorWorkAreaSource::MonitorInfo:
        return L"monitor_info";

    case MonitorWorkAreaSource::MonitorInfoClamped:
        return L"monitor_info_clamped";

    case MonitorWorkAreaSource::MonitorInfoAndSystemIntersection:
        return L"monitor_info_and_spi_intersection";

    case MonitorWorkAreaSource::SystemParametersInfo:
        return L"system_parameters_info";

    case MonitorWorkAreaSource::MonitorBoundsFallback:
        return L"monitor_bounds_fallback";

    default:
        return L"unknown";
    }
}
}  // namespace icon_mover
