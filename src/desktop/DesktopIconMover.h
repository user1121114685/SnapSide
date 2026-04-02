#pragma once

#include <string_view>

#include "core/Models.h"

namespace icon_mover
{
class DesktopIconMover final
{
public:
    static bool Execute(MoveTrigger trigger, std::wstring_view requested_monitor_id, MoveResult& result);

private:
    DesktopIconMover() = delete;
};
}  // namespace icon_mover
