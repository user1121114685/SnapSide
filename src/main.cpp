#include "app/App.h"

#include <string_view>

#include <shellapi.h>

#include "core/Constants.h"

namespace
{
icon_mover::LaunchOptions ParseLaunchOptions()
{
    icon_mover::LaunchOptions options;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        return options;
    }

    constexpr std::wstring_view kSetTargetMonitorPrefix = L"--set-target-monitor=";

    for (int index = 1; index < argc; ++index)
    {
        const std::wstring_view argument(argv[index]);

        if (::lstrcmpiW(argv[index], L"--log-test-error") == 0)
        {
            options.log_test_error = true;
            continue;
        }

        if (::lstrcmpiW(argv[index], icon_mover::kAutoStartLaunchArgument) == 0)
        {
            options.auto_start_launch = true;
            continue;
        }

        if (argument.starts_with(kSetTargetMonitorPrefix))
        {
            options.set_target_monitor_id = std::wstring(argument.substr(kSetTargetMonitorPrefix.size()));
        }
    }

    ::LocalFree(argv);
    return options;
}
}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    icon_mover::App app(instance, ParseLaunchOptions());
    return app.Run();
}

