#include "app/CommandHandler.h"

#include <string>
#include <utility>

#include "autostart/AutoStartManager.h"
#include "common/Logger.h"
#include "config/ConfigManager.h"
#include "monitor/MonitorManager.h"

namespace icon_mover
{
namespace
{
std::wstring BoolToString(bool value)
{
    return value ? L"true" : L"false";
}

const wchar_t* ToString(TargetMonitorResolution resolution) noexcept
{
    switch (resolution)
    {
    case TargetMonitorResolution::Unresolved:
        return L"unresolved";

    case TargetMonitorResolution::ConfiguredMonitor:
        return L"configured_monitor";

    case TargetMonitorResolution::FirstSecondaryMonitor:
        return L"first_secondary_monitor";

    case TargetMonitorResolution::PrimaryMonitorFallback:
        return L"primary_monitor_fallback";

    case TargetMonitorResolution::Unavailable:
        return L"unavailable";

    default:
        return L"unknown";
    }
}

const wchar_t* ToString(TrayCommandType command_type) noexcept
{
    switch (command_type)
    {
    case TrayCommandType::MoveIconsNow:
        return L"move_icons_now";

    case TrayCommandType::ExtendDisplays:
        return L"extend_displays";

    case TrayCommandType::ToggleAutoStart:
        return L"toggle_auto_start";

    case TrayCommandType::SelectTargetMonitor:
        return L"select_target_monitor";

    case TrayCommandType::SetTrayMenuLanguage:
        return L"set_tray_menu_language";

    case TrayCommandType::ExitApplication:
        return L"exit";

    default:
        return L"unknown";
    }
}

const wchar_t* ToString(TrayMenuLanguage tray_menu_language) noexcept
{
    switch (tray_menu_language)
    {
    case TrayMenuLanguage::ChineseSimplified:
        return L"zh-CN";

    case TrayMenuLanguage::English:
    default:
        return L"en";
    }
}
}  // namespace

bool CommandHandler::RefreshAutoStartState(AppState& state, std::wstring_view log_phase)
{
    AutoStartStatus auto_start_status;
    if (!AutoStartManager::Query(auto_start_status))
    {
        state.auto_start_enabled = false;
        state.auto_start_status_available = false;
        Logger::Warning(log_phase, L"Auto-start status query failed; the tray toggle will be disabled until it can be read again.");
        return false;
    }

    state.auto_start_enabled = auto_start_status.enabled && auto_start_status.matches_current_executable;
    state.auto_start_status_available = true;

    std::wstring message = L"Auto-start status refreshed. enabled=\"";
    message += BoolToString(auto_start_status.enabled);
    message += L"\" matches_current_executable=\"";
    message += BoolToString(auto_start_status.matches_current_executable);
    message += L"\" effective_enabled=\"";
    message += BoolToString(state.auto_start_enabled);
    message += L"\" command_line=\"";
    message += auto_start_status.command_line;
    message += L"\"";
    Logger::Info(log_phase, message);
    return true;
}

bool CommandHandler::RefreshMonitorState(AppState& state, std::wstring_view log_phase)
{
    MonitorTopology topology;
    if (!MonitorManager::RefreshTopology(topology))
    {
        state.monitor_topology = {};
        state.target_monitor = {};
        state.target_monitor.requested_monitor_id = state.config.target_monitor_id;
        Logger::Warning(log_phase, L"Monitor topology refresh failed; the target monitor tray submenu will show no selectable secondary monitors.");
        return false;
    }

    state.monitor_topology = std::move(topology);
    state.target_monitor = MonitorManager::ResolveTargetMonitor(state.monitor_topology, state.config.target_monitor_id);

    std::wstring message = L"Tray monitor state refreshed. requested_monitor_id=\"";
    message += state.target_monitor.requested_monitor_id;
    message += L"\" resolved_monitor_id=\"";
    message += state.target_monitor.resolved_monitor_id;
    message += L"\" resolution=\"";
    message += ToString(state.target_monitor.resolution);
    message += L"\" monitor_count=";
    message += std::to_wstring(static_cast<unsigned long long>(state.monitor_topology.monitors.size()));
    Logger::Info(log_phase, message);
    return true;
}

bool CommandHandler::RefreshTrayState(AppState& state, std::wstring_view log_phase)
{
    const bool monitor_state_ready = RefreshMonitorState(state, log_phase);
    const bool auto_start_state_ready = RefreshAutoStartState(state, log_phase);
    return monitor_state_ready && auto_start_state_ready;
}

bool CommandHandler::HandleTrayCommand(
    const TrayCommand& command,
    AppState& state,
    std::wstring_view log_phase,
    CommandDispatchResult& result)
{
    result = {};

    std::wstring message = L"Handling tray command. command=\"";
    message += ToString(command.type);
    message += L"\"";
    if (!command.target_monitor_id.empty())
    {
        message += L" target_monitor_id=\"";
        message += command.target_monitor_id;
        message += L"\"";
    }

    if (command.tray_menu_language.has_value())
    {
        message += L" tray_menu_language=\"";
        message += ToString(*command.tray_menu_language);
        message += L"\"";
    }
    Logger::Info(log_phase, message);

    switch (command.type)
    {
    case TrayCommandType::MoveIconsNow:
        result.should_execute_move = true;
        result.move_trigger = MoveTrigger::ManualCommand;
        return true;

    case TrayCommandType::ExtendDisplays:
        result.should_extend_displays = true;
        return true;

    case TrayCommandType::ToggleAutoStart:
    {
        if (!state.auto_start_status_available)
        {
            Logger::Warning(log_phase, L"Ignoring the tray auto-start toggle because the current Run registry state is unavailable.");
            return false;
        }

        const bool requested_enabled = !state.auto_start_enabled;
        Logger::Info(
            log_phase,
            L"Applying tray auto-start toggle. requested_enabled=\"" + BoolToString(requested_enabled) +
                L"\" previous_enabled=\"" + BoolToString(state.auto_start_enabled) + L"\"");
        if (!AutoStartManager::SetEnabled(requested_enabled))
        {
            static_cast<void>(RefreshAutoStartState(state, log_phase));
            Logger::Warning(log_phase, L"Tray auto-start toggle failed; the effective state was refreshed from the Run registry.");
            return false;
        }

        const bool refreshed = RefreshAutoStartState(state, log_phase);
        if (refreshed)
        {
            Logger::Info(
                log_phase,
                L"Tray auto-start toggle completed. effective_enabled=\"" + BoolToString(state.auto_start_enabled) + L"\"");
        }

        return refreshed;
    }

    case TrayCommandType::SelectTargetMonitor:
    {
        const MonitorInfo* monitor = MonitorManager::FindMonitorById(state.monitor_topology, command.target_monitor_id);
        if (monitor == nullptr)
        {
            Logger::Warning(
                log_phase,
                L"Ignoring the tray target monitor command because the selected monitor is no longer available. target_monitor_id=\"" +
                    command.target_monitor_id + L"\"");
            static_cast<void>(RefreshMonitorState(state, log_phase));
            return false;
        }

        if (monitor->is_primary)
        {
            Logger::Warning(
                log_phase,
                L"Ignoring the tray target monitor command because primary monitors are not selectable. target_monitor_id=\"" +
                    command.target_monitor_id + L"\"");
            static_cast<void>(RefreshMonitorState(state, log_phase));
            return false;
        }

        if (!ConfigManager::SetTargetMonitorId(state.config, command.target_monitor_id))
        {
            Logger::Warning(
                log_phase,
                L"Failed to persist the target monitor selected from the tray menu. target_monitor_id=\"" +
                    command.target_monitor_id + L"\"");
            static_cast<void>(RefreshMonitorState(state, log_phase));
            return false;
        }

        state.target_monitor = MonitorManager::ResolveTargetMonitor(state.monitor_topology, state.config.target_monitor_id);
        Logger::Info(
            log_phase,
            L"Tray target monitor selection persisted. requested_monitor_id=\"" + command.target_monitor_id +
                L"\" resolved_monitor_id=\"" + state.target_monitor.resolved_monitor_id + L"\" resolution=\"" +
                ToString(state.target_monitor.resolution) + L"\"");
        result.should_execute_move = true;
        result.move_trigger = MoveTrigger::TargetMonitorChanged;
        return true;
    }

    case TrayCommandType::SetTrayMenuLanguage:
    {
        result.should_execute_move = false;
        result.should_exit = false;

        if (!command.tray_menu_language.has_value())
        {
            Logger::Warning(log_phase, L"Ignoring the tray menu language command because the requested language is missing.");
            return false;
        }

        if (!ConfigManager::SetTrayMenuLanguage(state.config, *command.tray_menu_language))
        {
            Logger::Warning(
                log_phase,
                L"Failed to persist the tray menu language selected from the tray menu. tray_menu_language=\"" +
                    std::wstring(ToString(*command.tray_menu_language)) + L"\"");
            return false;
        }

        Logger::Info(
            log_phase,
            L"Tray menu language selection persisted. tray_menu_language=\"" +
                std::wstring(ToString(state.config.tray_menu_language)) + L"\"");
        return true;
    }

    case TrayCommandType::ExitApplication:
        result.should_exit = true;
        return true;

    default:
        Logger::Warning(log_phase, L"Received an unknown tray command type.");
        return false;
    }
}
}  // namespace icon_mover
