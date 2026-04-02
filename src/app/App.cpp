#include "app/App.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <commctrl.h>

#include "common/Logger.h"
#include "config/ConfigManager.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kBootstrapPhase[] = L"bootstrap";
constexpr wchar_t kRuntimePhase[] = L"runtime";
constexpr wchar_t kShutdownPhase[] = L"shutdown";

const wchar_t* TimerName(TimerId timer_id)
{
    switch (timer_id)
    {
    case TimerId::ExplorerRetry:
        return L"explorer_retry";

    case TimerId::FirstAutoMove:
        return L"first_auto_move";

    case TimerId::DisplayChangeDebounce:
        return L"display_change_debounce";

    default:
        return L"unknown";
    }
}

void SetTimerArmedState(TimerState& timer_state, TimerId timer_id, bool armed) noexcept
{
    switch (timer_id)
    {
    case TimerId::ExplorerRetry:
        timer_state.explorer_retry_armed = armed;
        return;

    case TimerId::FirstAutoMove:
        timer_state.first_auto_move_armed = armed;
        return;

    case TimerId::DisplayChangeDebounce:
        timer_state.display_change_debounce_armed = armed;
        return;

    default:
        return;
    }
}
}  // namespace

App::App(HINSTANCE instance, LaunchOptions options) noexcept
    : instance_(instance),
      launch_options_(std::move(options)),
      exit_code_(EXIT_SUCCESS),
      shutdown_requested_(false)
{
}

int App::Run()
{
    state_.lifecycle = AppLifecycleState::Bootstrap;

    if (!Initialize())
    {
        state_.lifecycle = AppLifecycleState::ShuttingDown;
        Logger::Error(kBootstrapPhase, L"Application initialization failed.");
        return EXIT_FAILURE;
    }

    if (launch_options_.set_target_monitor_id.has_value())
    {
        state_.lifecycle = AppLifecycleState::ShuttingDown;
        Logger::Info(kShutdownPhase, L"Startup configuration update completed; exiting without entering the message loop.");
        return EXIT_SUCCESS;
    }

    return MessageLoop();
}

bool App::Initialize()
{
    const bool logger_ready = Logger::Initialize();
    if (logger_ready)
    {
        std::wstring message = L"Logger initialized. log_path=\"";
        message += Logger::GetLogFilePath();
        message += L"\"";
        Logger::Info(kBootstrapPhase, message);
    }

    if (launch_options_.log_test_error)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kBootstrapPhase, L"Controlled error path requested via --log-test-error.");
    }

    const bool config_loaded_cleanly = LoadConfiguration();
    if (!config_loaded_cleanly)
    {
        Logger::Warning(kBootstrapPhase, L"Configuration loaded with fallback defaults or recovery.");
    }

    if (!ApplyStartupConfigOverrides())
    {
        return false;
    }

    if (launch_options_.set_target_monitor_id.has_value())
    {
        return true;
    }

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;

    if (::InitCommonControlsEx(&controls) == FALSE)
    {
        Logger::Warning(kBootstrapPhase, L"InitCommonControlsEx reported failure; continuing startup.");
    }

    if (!CreateMessageWindow())
    {
        return false;
    }

    state_.lifecycle = AppLifecycleState::Running;
    Logger::Info(kBootstrapPhase, L"Application bootstrap completed.");
    return true;
}

bool App::LoadConfiguration()
{
    const bool loaded_cleanly = ConfigManager::Load(state_.config);
    state_.last_execution_time = state_.config.last_successful_move_time;
    state_.target_monitor.requested_monitor_id = state_.config.target_monitor_id;

    std::wstring message = L"Configuration ready. target_monitor_id=\"";
    message += state_.config.target_monitor_id;
    message += L"\"";
    Logger::Info(kBootstrapPhase, message);
    return loaded_cleanly;
}

bool App::ApplyStartupConfigOverrides()
{
    if (!launch_options_.set_target_monitor_id.has_value())
    {
        return true;
    }

    if (!ConfigManager::SetTargetMonitorId(state_.config, *launch_options_.set_target_monitor_id))
    {
        Logger::Error(kBootstrapPhase, L"Failed to persist the startup target monitor override.");
        return false;
    }

    state_.target_monitor.requested_monitor_id = state_.config.target_monitor_id;

    std::wstring message;
    if (state_.config.target_monitor_id.empty())
    {
        message = L"Startup target monitor override cleared and persisted.";
    }
    else
    {
        message = L"Startup target monitor override persisted. target_monitor_id=\"";
        message += state_.config.target_monitor_id;
        message += L"\"";
    }

    Logger::Info(kBootstrapPhase, message);
    return true;
}

bool App::CreateMessageWindow()
{
    if (!message_window_.Create(instance_, this))
    {
        Logger::LogLastError(LogLevel::Error, kBootstrapPhase, L"Failed to create the hidden message window.");
        return false;
    }

    std::wstring message = L"Hidden message window created. hwnd=";
    message += std::to_wstring(reinterpret_cast<UINT_PTR>(message_window_.Handle()));
    Logger::Info(kBootstrapPhase, message);
    return true;
}

int App::MessageLoop()
{
    MSG message = {};

    while (true)
    {
        const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
        if (result == -1)
        {
            Logger::LogLastError(LogLevel::Error, kRuntimePhase, L"GetMessageW failed.");
            return EXIT_FAILURE;
        }

        if (result == 0)
        {
            Logger::Info(kShutdownPhase, L"Message loop exited.");
            return static_cast<int>(message.wParam);
        }

        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

bool App::ArmTimer(TimerId timer_id, UINT interval_ms)
{
    const HWND window = message_window_.Handle();
    if (window == nullptr)
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kRuntimePhase, L"Cannot arm a timer because the message window is unavailable.");
        return false;
    }

    if (::SetTimer(window, static_cast<UINT_PTR>(timer_id), interval_ms, nullptr) == 0)
    {
        std::wstring message = L"SetTimer failed. timer=\"";
        message += TimerName(timer_id);
        message += L"\"";
        Logger::LogLastError(LogLevel::Error, kRuntimePhase, message);
        return false;
    }

    SetTimerArmedState(state_.timers, timer_id, true);
    return true;
}

void App::DisarmTimer(TimerId timer_id) noexcept
{
    SetTimerArmedState(state_.timers, timer_id, false);

    const HWND window = message_window_.Handle();
    if (window != nullptr)
    {
        ::KillTimer(window, static_cast<UINT_PTR>(timer_id));
    }
}

void App::DisarmAllTimers() noexcept
{
    DisarmTimer(TimerId::ExplorerRetry);
    DisarmTimer(TimerId::FirstAutoMove);
    DisarmTimer(TimerId::DisplayChangeDebounce);
}

void App::HandleTimer(UINT_PTR timer_id)
{
    const auto managed_timer_id = static_cast<TimerId>(timer_id);

    switch (managed_timer_id)
    {
    case TimerId::ExplorerRetry:
    case TimerId::FirstAutoMove:
    case TimerId::DisplayChangeDebounce:
    {
        DisarmTimer(managed_timer_id);

        std::wstring message = L"Managed timer elapsed. timer=\"";
        message += TimerName(managed_timer_id);
        message += L"\"";
        Logger::Info(kRuntimePhase, message);
        return;
    }

    default:
        break;
    }

    std::wstring message = L"Received an unexpected WM_TIMER message. timer_id=";
    message += std::to_wstring(timer_id);
    Logger::Warning(kRuntimePhase, message);

    const HWND window = message_window_.Handle();
    if (window != nullptr)
    {
        ::KillTimer(window, timer_id);
    }
}

void App::HandleDisplayChange(UINT bits_per_pixel, int width, int height)
{
    std::wstring message = L"WM_DISPLAYCHANGE received. bits_per_pixel=";
    message += std::to_wstring(bits_per_pixel);
    message += L" width=";
    message += std::to_wstring(width);
    message += L" height=";
    message += std::to_wstring(height);
    Logger::Info(kRuntimePhase, message);

    if (!ArmTimer(TimerId::DisplayChangeDebounce, kDisplayChangeDebounceMs))
    {
        Logger::Warning(kRuntimePhase, L"Display change debounce timer was not armed.");
    }
}

void App::BeginShutdown(int exit_code, std::wstring_view reason)
{
    if (shutdown_requested_)
    {
        return;
    }

    shutdown_requested_ = true;
    exit_code_ = exit_code;
    state_.lifecycle = AppLifecycleState::ShuttingDown;
    DisarmAllTimers();
    Logger::Info(kShutdownPhase, reason);

    const HWND window = message_window_.Handle();
    if (window != nullptr)
    {
        ::DestroyWindow(window);
    }
    else
    {
        ::PostQuitMessage(exit_code_);
    }
}

std::optional<LRESULT> App::OnMessageWindowMessage(HWND, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_CREATE:
        Logger::Info(kBootstrapPhase, L"Hidden message window is ready.");
        return 0;

    case WM_TIMER:
        HandleTimer(static_cast<UINT_PTR>(w_param));
        return 0;

    case WM_DISPLAYCHANGE:
        HandleDisplayChange(static_cast<UINT>(w_param), LOWORD(l_param), HIWORD(l_param));
        return 0;

    case kTrayIconMessage:
        Logger::Info(kRuntimePhase, L"Tray callback message received without an active tray icon.");
        return 0;

    case kShutdownApplicationMessage:
        BeginShutdown(static_cast<int>(w_param), L"Shutdown requested through the custom application message.");
        return 0;

    case WM_CLOSE:
        BeginShutdown(EXIT_SUCCESS, L"Shutdown requested through WM_CLOSE.");
        return 0;

    case WM_QUERYENDSESSION:
        Logger::Info(kShutdownPhase, L"WM_QUERYENDSESSION received.");
        return static_cast<LRESULT>(TRUE);

    case WM_ENDSESSION:
        if (w_param != FALSE)
        {
            BeginShutdown(EXIT_SUCCESS, L"Windows session end was confirmed.");
        }
        return 0;

    case WM_DESTROY:
        state_.lifecycle = AppLifecycleState::ShuttingDown;
        Logger::Info(kShutdownPhase, L"Hidden message window destroyed; posting WM_QUIT.");
        ::PostQuitMessage(exit_code_);
        return 0;

    default:
        return std::nullopt;
    }
}
}  // namespace icon_mover

