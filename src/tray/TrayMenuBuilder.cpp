#include "tray/TrayMenuBuilder.h"

#include <string>

#include "resource/resource.h"

namespace icon_mover
{
namespace
{
bool AppendMenuItem(HMENU menu, UINT flags, UINT_PTR identifier, const wchar_t* text)
{
    return ::AppendMenuW(menu, flags, identifier, text) != FALSE;
}

std::wstring BuildMonitorMenuLabel(const MonitorInfo& monitor)
{
    std::wstring label = L"Display ";
    label += std::to_wstring(monitor.display_index);
    label += L" (Secondary)";

    if (!monitor.device_name.empty())
    {
        label += L" - ";
        label += monitor.device_name;
    }

    return label;
}

bool PopulateTargetMonitorMenu(
    HMENU target_menu,
    const TrayMenuContext& context,
    std::vector<TrayMenuMonitorCommand>& monitor_commands)
{
    const MonitorTopology* topology = context.monitor_topology;
    const TargetMonitorSelection* selection = context.target_monitor;
    const std::wstring resolved_monitor_id =
        selection != nullptr ? selection->resolved_monitor_id : std::wstring();

    bool appended_selectable_monitor = false;
    if (topology != nullptr)
    {
        for (const MonitorInfo& monitor : topology->monitors)
        {
            if (monitor.is_primary)
            {
                continue;
            }

            const UINT command_id = ID_TRAY_TARGET_MONITOR_BASE + static_cast<UINT>(monitor_commands.size());
            if (command_id > ID_TRAY_TARGET_MONITOR_LAST)
            {
                ::SetLastError(ERROR_NOT_SUPPORTED);
                return false;
            }

            UINT item_flags = MF_STRING;
            if (monitor.monitor_id == resolved_monitor_id)
            {
                item_flags |= MF_CHECKED;
            }

            const std::wstring label = BuildMonitorMenuLabel(monitor);
            if (!AppendMenuItem(target_menu, item_flags, command_id, label.c_str()))
            {
                return false;
            }

            monitor_commands.push_back(TrayMenuMonitorCommand{command_id, monitor.monitor_id});
            appended_selectable_monitor = true;
        }
    }

    if (appended_selectable_monitor)
    {
        return true;
    }

    return AppendMenuItem(target_menu, MF_STRING | MF_GRAYED, 0, L"No secondary monitor available");
}
}  // namespace

bool TrayMenuBuilder::PopulateMenu(
    HMENU menu,
    const TrayMenuContext& context,
    std::vector<TrayMenuMonitorCommand>& monitor_commands)
{
    monitor_commands.clear();

    if (!AppendMenuItem(menu, MF_STRING, ID_TRAY_MOVE_ICONS_NOW, L"Move Icons Now"))
    {
        return false;
    }

    HMENU target_monitor_menu = ::CreatePopupMenu();
    if (target_monitor_menu == nullptr)
    {
        return false;
    }

    if (!PopulateTargetMonitorMenu(target_monitor_menu, context, monitor_commands))
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(target_monitor_menu);
        ::SetLastError(error_code);
        return false;
    }

    if (!AppendMenuItem(
            menu,
            MF_POPUP | MF_STRING,
            reinterpret_cast<UINT_PTR>(target_monitor_menu),
            L"Set Target Monitor"))
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(target_monitor_menu);
        ::SetLastError(error_code);
        return false;
    }

    target_monitor_menu = nullptr;

    UINT auto_start_flags = MF_STRING;
    if (context.auto_start_enabled)
    {
        auto_start_flags |= MF_CHECKED;
    }

    if (!context.auto_start_status_available)
    {
        auto_start_flags |= MF_GRAYED;
    }

    if (!AppendMenuItem(menu, auto_start_flags, ID_TRAY_TOGGLE_AUTO_START, L"Launch at Startup"))
    {
        return false;
    }

    if (!AppendMenuItem(menu, MF_SEPARATOR, 0, nullptr))
    {
        return false;
    }

    if (!AppendMenuItem(menu, MF_STRING, ID_TRAY_EXIT, L"Exit"))
    {
        return false;
    }

    return true;
}
}  // namespace icon_mover
