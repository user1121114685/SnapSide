#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/Models.h"

namespace icon_mover
{
class ConfigManager final
{
public:
    static bool Load(AppConfig& config);
    static bool Save(const AppConfig& config);
    static bool SetTargetMonitorId(AppConfig& config, std::wstring_view target_monitor_id);
    static bool SetLastSuccessfulMoveTime(AppConfig& config, std::optional<Timestamp> timestamp);
    static std::wstring GetConfigFilePath();

private:
    ConfigManager() = delete;
};
}  // namespace icon_mover
