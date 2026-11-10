#pragma once

#include <windows.h>

namespace icon_mover
{
enum class TimerId : UINT_PTR
{
    ExplorerRetry = 1,
    FirstAutoMove = 2,
    DisplayChangeDebounce = 3
};

inline constexpr UINT kTrayIconMessage = WM_APP + 1;
inline constexpr UINT kShutdownApplicationMessage = WM_APP + 2;
inline constexpr int kExtendDisplaysHotkeyId = 1;

inline constexpr UINT kExplorerRetryIntervalMs = 1000;
inline constexpr UINT kFirstAutoMoveDelayMs = 1500;
inline constexpr UINT kFirstAutoMoveAfterDisplayExtendDelayMs = 4000;
inline constexpr UINT kDisplayChangeDebounceMs = 2000;

inline constexpr wchar_t kAutoStartLaunchArgument[] = L"--autostart";

inline constexpr wchar_t kConfigFileName[] = L"config.ini";
inline constexpr wchar_t kConfigSectionGeneral[] = L"general";
inline constexpr wchar_t kConfigKeyTargetMonitorId[] = L"target_monitor_id";
inline constexpr wchar_t kConfigKeyLastSuccessfulMoveUtc[] = L"last_successful_move_utc";
inline constexpr wchar_t kConfigKeyLogLevel[] = L"log_level";
inline constexpr wchar_t kConfigKeyTrayMenuLanguage[] = L"tray_menu_language";
}  // namespace icon_mover
