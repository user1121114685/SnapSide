#pragma once

#include <string>
#include <string_view>

#include <windows.h>

namespace icon_mover
{
enum class LogLevel
{
    Info,
    Warning,
    Error
};

class Logger final
{
public:
    static bool Initialize();

    static void Write(LogLevel level, std::wstring_view phase, std::wstring_view message);
    static void Info(std::wstring_view phase, std::wstring_view message);
    static void Warning(std::wstring_view phase, std::wstring_view message);
    static void Error(std::wstring_view phase, std::wstring_view message);
    static void LogLastError(LogLevel level, std::wstring_view phase, std::wstring_view context);

    static std::wstring GetLogFilePath();

private:
    Logger() = delete;
};
}  // namespace icon_mover

