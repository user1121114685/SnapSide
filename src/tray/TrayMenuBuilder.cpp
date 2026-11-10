#include "tray/TrayMenuBuilder.h"

#include <string>

#include "resource/resource.h"

namespace icon_mover
{
namespace
{
enum class TrayMenuText
{
    MoveIconsNow,
    ExtendDisplays,
    SetTargetMonitor,
    LaunchAtStartup,
    Exit,
    NoSecondaryMonitorAvailable,
    LanguageSubmenu,
    EnglishOption,
    ChineseSimplifiedOption
};

bool AppendMenuItem(HMENU menu, UINT flags, UINT_PTR identifier, const wchar_t* text)
{
    return ::AppendMenuW(menu, flags, identifier, text) != FALSE;
}

const wchar_t* Text(TrayMenuLanguage tray_menu_language, TrayMenuText text) noexcept
{
    switch (tray_menu_language)
    {
    case TrayMenuLanguage::ChineseSimplified:
        switch (text)
        {
        case TrayMenuText::MoveIconsNow:
            return L"立即移动图标";

        case TrayMenuText::ExtendDisplays:
            return L"扩展这些显示器\tCtrl+F11";

        case TrayMenuText::SetTargetMonitor:
            return L"设置目标显示器";

        case TrayMenuText::LaunchAtStartup:
            return L"开机自启";

        case TrayMenuText::Exit:
            return L"退出";

        case TrayMenuText::NoSecondaryMonitorAvailable:
            return L"没有可用副显示器";

        case TrayMenuText::LanguageSubmenu:
            return L"Language / 语言";

        case TrayMenuText::EnglishOption:
            return L"English";

        case TrayMenuText::ChineseSimplifiedOption:
            return L"中文";
        }
        break;

    case TrayMenuLanguage::English:
    default:
        switch (text)
        {
        case TrayMenuText::MoveIconsNow:
            return L"Move Icons Now";

        case TrayMenuText::ExtendDisplays:
            return L"Extend These Displays\tCtrl+F11";

        case TrayMenuText::SetTargetMonitor:
            return L"Set Target Monitor";

        case TrayMenuText::LaunchAtStartup:
            return L"Launch at Startup";

        case TrayMenuText::Exit:
            return L"Exit";

        case TrayMenuText::NoSecondaryMonitorAvailable:
            return L"No secondary monitor available";

        case TrayMenuText::LanguageSubmenu:
            return L"Language / 语言";

        case TrayMenuText::EnglishOption:
            return L"English";

        case TrayMenuText::ChineseSimplifiedOption:
            return L"中文";
        }
        break;
    }

    return L"";
}

std::wstring BuildMonitorMenuLabel(const MonitorInfo& monitor, TrayMenuLanguage tray_menu_language)
{
    std::wstring label;
    if (tray_menu_language == TrayMenuLanguage::ChineseSimplified)
    {
        label = L"显示器 ";
        label += std::to_wstring(monitor.display_index);
        label += L"（副屏）";
    }
    else
    {
        label = L"Display ";
        label += std::to_wstring(monitor.display_index);
        label += L" (Secondary)";
    }

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

            const std::wstring label = BuildMonitorMenuLabel(monitor, context.tray_menu_language);
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

    return AppendMenuItem(
        target_menu,
        MF_STRING | MF_GRAYED,
        0,
        Text(context.tray_menu_language, TrayMenuText::NoSecondaryMonitorAvailable));
}

bool PopulateLanguageMenu(HMENU language_menu, const TrayMenuContext& context)
{
    UINT english_flags = MF_STRING;
    if (context.tray_menu_language == TrayMenuLanguage::English)
    {
        english_flags |= MF_CHECKED;
    }

    if (!AppendMenuItem(
            language_menu,
            english_flags,
            ID_TRAY_SET_LANGUAGE_ENGLISH,
            Text(context.tray_menu_language, TrayMenuText::EnglishOption)))
    {
        return false;
    }

    UINT chinese_flags = MF_STRING;
    if (context.tray_menu_language == TrayMenuLanguage::ChineseSimplified)
    {
        chinese_flags |= MF_CHECKED;
    }

    return AppendMenuItem(
        language_menu,
        chinese_flags,
        ID_TRAY_SET_LANGUAGE_CHINESE_SIMPLIFIED,
        Text(context.tray_menu_language, TrayMenuText::ChineseSimplifiedOption));
}
}  // namespace

bool TrayMenuBuilder::PopulateMenu(
    HMENU menu,
    const TrayMenuContext& context,
    std::vector<TrayMenuMonitorCommand>& monitor_commands)
{
    monitor_commands.clear();

    if (!AppendMenuItem(
            menu,
            MF_STRING,
            ID_TRAY_MOVE_ICONS_NOW,
            Text(context.tray_menu_language, TrayMenuText::MoveIconsNow)))
    {
        return false;
    }

    if (!AppendMenuItem(
            menu,
            MF_STRING,
            ID_TRAY_EXTEND_DISPLAYS,
            Text(context.tray_menu_language, TrayMenuText::ExtendDisplays)))
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
            Text(context.tray_menu_language, TrayMenuText::SetTargetMonitor)))
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(target_monitor_menu);
        ::SetLastError(error_code);
        return false;
    }

    target_monitor_menu = nullptr;

    HMENU language_menu = ::CreatePopupMenu();
    if (language_menu == nullptr)
    {
        return false;
    }

    if (!PopulateLanguageMenu(language_menu, context))
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(language_menu);
        ::SetLastError(error_code);
        return false;
    }

    if (!AppendMenuItem(
            menu,
            MF_POPUP | MF_STRING,
            reinterpret_cast<UINT_PTR>(language_menu),
            Text(context.tray_menu_language, TrayMenuText::LanguageSubmenu)))
    {
        const DWORD error_code = ::GetLastError();
        ::DestroyMenu(language_menu);
        ::SetLastError(error_code);
        return false;
    }

    language_menu = nullptr;

    UINT auto_start_flags = MF_STRING;
    if (context.auto_start_enabled)
    {
        auto_start_flags |= MF_CHECKED;
    }

    if (!context.auto_start_status_available)
    {
        auto_start_flags |= MF_GRAYED;
    }

    if (!AppendMenuItem(
            menu,
            auto_start_flags,
            ID_TRAY_TOGGLE_AUTO_START,
            Text(context.tray_menu_language, TrayMenuText::LaunchAtStartup)))
    {
        return false;
    }

    if (!AppendMenuItem(menu, MF_SEPARATOR, 0, nullptr))
    {
        return false;
    }

    if (!AppendMenuItem(menu, MF_STRING, ID_TRAY_EXIT, Text(context.tray_menu_language, TrayMenuText::Exit)))
    {
        return false;
    }

    return true;
}
}  // namespace icon_mover
