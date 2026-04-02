#pragma once

#include <vector>

#include <windows.h>

#include "tray/TrayManager.h"

namespace icon_mover
{
class TrayMenuBuilder final
{
public:
    static bool PopulateMenu(
        HMENU menu,
        const TrayMenuContext& context,
        std::vector<TrayMenuMonitorCommand>& monitor_commands);

private:
    TrayMenuBuilder() = delete;
};
}  // namespace icon_mover
