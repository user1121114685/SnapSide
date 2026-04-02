#include "monitor/MonitorManager.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/Logger.h"
#include "common/Win32Error.h"
#include "monitor/MonitorWorkArea.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kMonitorPhase[] = L"monitor";

struct MonitorEnumContext final
{
    MonitorTopology topology;
    DWORD last_error = ERROR_SUCCESS;
    bool aborted = false;
    bool logged_multiple_primary = false;
};

std::wstring BoolToString(bool value)
{
    return value ? L"true" : L"false";
}

std::wstring RectToString(const RECT& rect)
{
    std::wstring text = L"[";
    text += std::to_wstring(rect.left);
    text += L",";
    text += std::to_wstring(rect.top);
    text += L",";
    text += std::to_wstring(rect.right);
    text += L",";
    text += std::to_wstring(rect.bottom);
    text += L"]";
    return text;
}

std::wstring BuildMonitorIdentifier(std::wstring_view device_name, UINT display_index)
{
    if (!device_name.empty())
    {
        return std::wstring(device_name);
    }

    std::wstring fallback = L"DISPLAY-";
    fallback += std::to_wstring(display_index);
    return fallback;
}

std::optional<std::size_t> FindMonitorIndexById(const MonitorTopology& topology, std::wstring_view monitor_id) noexcept
{
    if (monitor_id.empty())
    {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < topology.monitors.size(); ++index)
    {
        if (topology.monitors[index].monitor_id == monitor_id)
        {
            return index;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> FindFirstSecondaryMonitorIndex(const MonitorTopology& topology) noexcept
{
    for (std::size_t index = 0; index < topology.monitors.size(); ++index)
    {
        if (!topology.monitors[index].is_primary)
        {
            return index;
        }
    }

    return std::nullopt;
}

std::wstring BuildMonitorSummary(const MonitorInfo& monitor)
{
    std::wstring summary = L"display_index=";
    summary += std::to_wstring(monitor.display_index);
    summary += L" monitor_id=\"";
    summary += monitor.monitor_id;
    summary += L"\" device_name=\"";
    summary += monitor.device_name;
    summary += L"\" is_primary=";
    summary += BoolToString(monitor.is_primary);
    summary += L" monitor_rect=";
    summary += RectToString(monitor.monitor_rect);
    summary += L" raw_work_area_rect=";
    summary += RectToString(monitor.raw_work_area_rect);
    return summary;
}

std::wstring BuildWorkAreaResolutionSummary(
    const MonitorInfo& monitor,
    const MonitorWorkAreaResult& result,
    const VirtualScreenInfo& virtual_screen)
{
    std::wstring summary = L"Resolved monitor work area. monitor_id=\"";
    summary += monitor.monitor_id;
    summary += L"\" display_index=";
    summary += std::to_wstring(monitor.display_index);
    summary += L" source=";
    summary += ToString(result.source);
    summary += L" monitor_rect=";
    summary += RectToString(monitor.monitor_rect);
    summary += L" raw_work_area_rect=";
    summary += RectToString(monitor.raw_work_area_rect);
    summary += L" work_area_rect=";
    summary += RectToString(result.resolved_rect);
    summary += L" system_work_area_checked=";
    summary += BoolToString(result.system_work_area_checked);
    summary += L" system_work_area_compatible=";
    summary += BoolToString(result.system_work_area_compatible);

    if (virtual_screen.has_system_work_area)
    {
        summary += L" system_work_area=";
        summary += RectToString(virtual_screen.system_work_area);
    }

    return summary;
}

void PopulateVirtualScreenInfo(VirtualScreenInfo& virtual_screen)
{
    const int virtual_left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    virtual_screen.bounds.left = virtual_left;
    virtual_screen.bounds.top = virtual_top;
    virtual_screen.bounds.right = virtual_left + virtual_width;
    virtual_screen.bounds.bottom = virtual_top + virtual_height;
    virtual_screen.monitor_count = ::GetSystemMetrics(SM_CMONITORS);
}

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor_handle, HDC, LPRECT, LPARAM parameter)
{
    auto* context = reinterpret_cast<MonitorEnumContext*>(parameter);
    if (context == nullptr)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    MONITORINFOEXW monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);

    if (::GetMonitorInfoW(monitor_handle, &monitor_info) == FALSE)
    {
        context->last_error = ::GetLastError();
        context->aborted = true;
        return FALSE;
    }

    MonitorInfo monitor = {};
    monitor.handle = monitor_handle;
    monitor.display_index = static_cast<UINT>(context->topology.monitors.size() + 1);
    monitor.device_name = monitor_info.szDevice;
    monitor.monitor_id = BuildMonitorIdentifier(monitor.device_name, monitor.display_index);
    monitor.is_primary = (monitor_info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    monitor.monitor_rect = monitor_info.rcMonitor;
    monitor.raw_work_area_rect = monitor_info.rcWork;
    monitor.work_area_rect = monitor_info.rcWork;

    if (monitor.is_primary)
    {
        if (!context->topology.primary_monitor_index.has_value())
        {
            context->topology.primary_monitor_index = context->topology.monitors.size();
        }
        else if (!context->logged_multiple_primary)
        {
            Logger::Warning(
                kMonitorPhase,
                L"Multiple monitors reported MONITORINFOF_PRIMARY. Secondary selection will continue to require !is_primary.");
            context->logged_multiple_primary = true;
        }
    }

    Logger::Info(kMonitorPhase, L"Enumerated monitor. " + BuildMonitorSummary(monitor));
    context->topology.monitors.push_back(std::move(monitor));
    return TRUE;
}

void LogTopologySummary(const MonitorTopology& topology)
{
    std::wstring summary = L"Monitor topology refreshed. monitors=";
    summary += std::to_wstring(static_cast<unsigned long long>(topology.monitors.size()));
    summary += L" primary_monitor_index=";

    if (topology.primary_monitor_index.has_value())
    {
        summary += std::to_wstring(static_cast<unsigned long long>(*topology.primary_monitor_index));
    }
    else
    {
        summary += L"none";
    }

    summary += L" virtual_bounds=";
    summary += RectToString(topology.virtual_screen.bounds);
    summary += L" system_work_area=";
    if (topology.virtual_screen.has_system_work_area)
    {
        summary += RectToString(topology.virtual_screen.system_work_area);
    }
    else
    {
        summary += L"[unavailable]";
    }
    summary += L" has_secondary_monitor=";
    summary += BoolToString(topology.virtual_screen.has_secondary_monitor);
    summary += L" system_monitor_count=";
    summary += std::to_wstring(topology.virtual_screen.monitor_count);
    Logger::Info(kMonitorPhase, summary);
}
}  // namespace

bool MonitorManager::RefreshTopology(MonitorTopology& topology)
{
    MonitorEnumContext context = {};
    PopulateVirtualScreenInfo(context.topology.virtual_screen);

    if (!RefreshSystemWorkArea(context.topology.virtual_screen))
    {
        Logger::LogLastError(LogLevel::Warning, kMonitorPhase, L"SystemParametersInfoW(SPI_GETWORKAREA) failed; monitor work area resolution will fall back to GetMonitorInfoW data.");
    }
    else
    {
        Logger::Info(
            kMonitorPhase,
            L"Captured system work area from SystemParametersInfoW(SPI_GETWORKAREA). system_work_area=" +
                RectToString(context.topology.virtual_screen.system_work_area));
    }

    if (::EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&context)) == FALSE)
    {
        const DWORD error_code = context.aborted ? context.last_error : ::GetLastError();
        ::SetLastError(error_code);
        Logger::LogLastError(LogLevel::Error, kMonitorPhase, L"EnumDisplayMonitors failed.");
        return false;
    }

    if (context.topology.monitors.empty())
    {
        ::SetLastError(ERROR_NOT_FOUND);
        Logger::LogLastError(LogLevel::Error, kMonitorPhase, L"No monitors were returned by EnumDisplayMonitors.");
        return false;
    }

    if (context.topology.virtual_screen.monitor_count <= 0)
    {
        context.topology.virtual_screen.monitor_count = static_cast<int>(context.topology.monitors.size());
    }

    context.topology.virtual_screen.has_secondary_monitor = FindFirstSecondaryMonitorIndex(context.topology).has_value();

    if (!context.topology.primary_monitor_index.has_value())
    {
        Logger::Warning(kMonitorPhase, L"No primary monitor flag was reported during enumeration.");
    }

    if (context.topology.virtual_screen.monitor_count != static_cast<int>(context.topology.monitors.size()))
    {
        std::wstring message = L"System monitor count differs from the enumerated monitor list. system_monitor_count=";
        message += std::to_wstring(context.topology.virtual_screen.monitor_count);
        message += L" enumerated_count=";
        message += std::to_wstring(static_cast<unsigned long long>(context.topology.monitors.size()));
        Logger::Warning(kMonitorPhase, message);
    }

    for (MonitorInfo& monitor : context.topology.monitors)
    {
        const MonitorWorkAreaResult resolved_work_area = ResolveMonitorWorkArea(monitor, context.topology.virtual_screen);
        monitor.work_area_rect = resolved_work_area.resolved_rect;
        Logger::Info(kMonitorPhase, BuildWorkAreaResolutionSummary(monitor, resolved_work_area, context.topology.virtual_screen));
    }

    LogTopologySummary(context.topology);
    topology = std::move(context.topology);
    return true;
}

TargetMonitorSelection MonitorManager::ResolveTargetMonitor(
    const MonitorTopology& topology,
    std::wstring_view requested_monitor_id)
{
    TargetMonitorSelection selection = {};
    selection.requested_monitor_id.assign(requested_monitor_id.data(), requested_monitor_id.size());

    const std::optional<std::size_t> secondary_index = FindFirstSecondaryMonitorIndex(topology);
    if (!secondary_index.has_value())
    {
        std::wstring message = L"No available target monitor because no non-primary monitor is present. monitor_count=";
        message += std::to_wstring(static_cast<unsigned long long>(topology.monitors.size()));
        if (!requested_monitor_id.empty())
        {
            message += L" requested_monitor_id=\"";
            message.append(requested_monitor_id.data(), requested_monitor_id.size());
            message += L"\"";
        }

        Logger::Warning(kMonitorPhase, message);
        selection.resolution = TargetMonitorResolution::Unavailable;
        return selection;
    }

    const std::optional<std::size_t> configured_index = FindMonitorIndexById(topology, requested_monitor_id);
    if (configured_index.has_value())
    {
        selection.resolved_monitor_id = topology.monitors[*configured_index].monitor_id;
        selection.resolution = TargetMonitorResolution::ConfiguredMonitor;

        std::wstring message = L"Resolved target monitor from configuration. monitor_id=\"";
        message += selection.resolved_monitor_id;
        message += L"\" is_primary=";
        message += BoolToString(topology.monitors[*configured_index].is_primary);
        Logger::Info(kMonitorPhase, message);
        return selection;
    }

    if (!requested_monitor_id.empty())
    {
        std::wstring message = L"Configured target monitor is unavailable. Falling back to the first non-primary monitor. requested_monitor_id=\"";
        message.append(requested_monitor_id.data(), requested_monitor_id.size());
        message += L"\"";
        Logger::Warning(kMonitorPhase, message);
    }

    selection.resolved_monitor_id = topology.monitors[*secondary_index].monitor_id;
    selection.resolution = TargetMonitorResolution::FirstSecondaryMonitor;

    std::wstring message = L"Resolved target monitor to the first non-primary monitor. monitor_id=\"";
    message += selection.resolved_monitor_id;
    message += L"\"";
    Logger::Info(kMonitorPhase, message);
    return selection;
}

const MonitorInfo* MonitorManager::FindMonitorById(const MonitorTopology& topology, std::wstring_view monitor_id) noexcept
{
    const std::optional<std::size_t> index = FindMonitorIndexById(topology, monitor_id);
    if (!index.has_value())
    {
        return nullptr;
    }

    return &topology.monitors[*index];
}
}  // namespace icon_mover
