#pragma once

#include <string_view>

#include "core/AppState.h"
#include "tray/TrayManager.h"

namespace icon_mover
{
struct CommandDispatchResult final
{
    bool should_execute_move = false;
    MoveTrigger move_trigger = MoveTrigger::ManualCommand;
    bool should_exit = false;
};

class CommandHandler final
{
public:
    static bool RefreshAutoStartState(AppState& state, std::wstring_view log_phase);
    static bool RefreshMonitorState(AppState& state, std::wstring_view log_phase);
    static bool RefreshTrayState(AppState& state, std::wstring_view log_phase);
    static bool HandleTrayCommand(
        const TrayCommand& command,
        AppState& state,
        std::wstring_view log_phase,
        CommandDispatchResult& result);

private:
    CommandHandler() = delete;
};
}  // namespace icon_mover
