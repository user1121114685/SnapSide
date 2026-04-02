#pragma once

#include <string>

namespace icon_mover
{
struct AutoStartStatus final
{
    bool enabled = false;
    bool matches_current_executable = false;
    std::wstring command_line;
};

class AutoStartManager final
{
public:
    static bool Query(AutoStartStatus& status);
    static bool SetEnabled(bool enabled);
    static bool Enable();
    static bool Disable();

private:
    AutoStartManager() = delete;
};
}  // namespace icon_mover
