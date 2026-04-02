#include "app/App.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <commctrl.h>

#include "app/CommandHandler.h"
#include "common/Logger.h"
#include "config/ConfigManager.h"
#include "desktop/DesktopIconMover.h"
#include "monitor/MonitorManager.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kBootstrapPhase[] = L"bootstrap";
constexpr wchar_t kRuntimePhase[] = L"runtime";
constexpr wchar_t kShutdownPhase[] = L"shutdown";
constexpr UINT kMaxDisplayChangeMoveAttempts = 4;
constexpr ULONGLONG kDisplayChangeMoveTimeoutMs = 15000;

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

const wchar_t* FirstAutoMoveStateName(FirstAutoMoveState state) noexcept
{
    switch (state)
    {
    case FirstAutoMoveState::Pending:
        return L"pending";

    case FirstAutoMoveState::WaitingForExplorer:
        return L"waiting_for_explorer";

    case FirstAutoMoveState::ReadyToRun:
        return L"ready_to_run";

    case FirstAutoMoveState::Running:
        return L"running";

    case FirstAutoMoveState::Completed:
        return L"completed";

    case FirstAutoMoveState::Failed:
        return L"failed";

    default:
        return L"unknown";
    }
}

const wchar_t* MoveOutcomeName(MoveOutcome outcome) noexcept
{
    switch (outcome)
    {
    case MoveOutcome::Success:
        return L"success";

    case MoveOutcome::Skipped:
        return L"skipped";

    case MoveOutcome::Failed:
        return L"failed";

    default:
        return L"unknown";
    }
}

const wchar_t* MoveTriggerName(MoveTrigger trigger) noexcept
{
    switch (trigger)
    {
    case MoveTrigger::StartupAuto:
        return L"startup_auto";

    case MoveTrigger::ManualCommand:
        return L"manual_command";

    case MoveTrigger::DisplayChange:
        return L"display_change";

    case MoveTrigger::TargetMonitorChanged:
        return L"target_monitor_changed";

    case MoveTrigger::ExplorerRecovered:
        return L"explorer_recovered";

    default:
        return L"unknown";
    }
}

const wchar_t* MoveSkipReasonName(MoveSkipReason reason) noexcept
{
    switch (reason)
    {
    case MoveSkipReason::None:
        return L"none";

    case MoveSkipReason::ExplorerNotReady:
        return L"explorer_not_ready";

    case MoveSkipReason::NoAvailableMonitor:
        return L"no_available_monitor";

    case MoveSkipReason::TargetMonitorUnavailable:
        return L"target_monitor_unavailable";

    case MoveSkipReason::NoDesktopIcons:
        return L"no_desktop_icons";

    default:
        return L"unknown";
    }
}

const wchar_t* TargetMonitorResolutionName(TargetMonitorResolution resolution) noexcept
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

bool ShouldRetryDisplayChangeMove(const MoveResult& move_result) noexcept
{
    return move_result.outcome == MoveOutcome::Failed &&
           (move_result.skip_reason == MoveSkipReason::ExplorerNotReady ||
            DesktopViewAccessor::IsTransientRefreshError(move_result.last_error));
}

bool IsExplorerReady(const AppState& state) noexcept
{
    return state.explorer.availability == ExplorerAvailability::Ready &&
           state.explorer.desktop_list_view_window != nullptr &&
           ::IsWindow(state.explorer.desktop_list_view_window) != FALSE;
}

const wchar_t* CurrentPhase(const AppState& state) noexcept
{
    return state.lifecycle == AppLifecycleState::Bootstrap ? kBootstrapPhase : kRuntimePhase;
}

const wchar_t* BoolName(bool value) noexcept
{
    return value ? L"true" : L"false";
}
}  // namespace

App::App(HINSTANCE instance, LaunchOptions options) noexcept
    : instance_(instance),
      launch_options_(std::move(options)),
      exit_code_(EXIT_SUCCESS),
      shutdown_requested_(false),
      pending_display_change_move_(false),
      suppress_immediate_display_change_move_(false),
      display_change_move_session_active_(false),
      display_change_generation_(0),
      display_change_started_tick_(0),
      display_change_move_attempt_count_(0)
{
}

int App::Run()
{
    state_.lifecycle = AppLifecycleState::Bootstrap;

    if (!Initialize())
    {
        state_.lifecycle = AppLifecycleState::ShuttingDown;
        Logger::Error(kBootstrapPhase, L"Application initialization failed.");
        BeginShutdown(EXIT_FAILURE, L"Shutting down after initialization failure.");
        return EXIT_FAILURE;
    }

    if (launch_options_.set_target_monitor_id.has_value())
    {
        state_.lifecycle = AppLifecycleState::ShuttingDown;
        BeginShutdown(EXIT_SUCCESS, L"Startup configuration update completed; exiting without entering the message loop.");
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

    Logger::Info(kBootstrapPhase, L"Application startup sequence begins.");

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

    if (!tray_manager_.Initialize(instance_, message_window_.Handle()))
    {
        Logger::Error(kBootstrapPhase, L"Failed to initialize the tray icon manager.");
        return false;
    }

    if (!LoadAutoStartStatus())
    {
        Logger::Warning(kBootstrapPhase, L"Auto-start status query failed; continuing with the cached disabled state.");
    }

    if (!StartExplorerReadinessWait())
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

bool App::LoadAutoStartStatus()
{
    return CommandHandler::RefreshAutoStartState(state_, kBootstrapPhase);
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

bool App::StartExplorerReadinessWait()
{
    state_.first_auto_move_state = FirstAutoMoveState::WaitingForExplorer;

    Logger::Info(kBootstrapPhase, L"Starting Explorer readiness wait.");

    const ExplorerAvailability previous_availability = state_.explorer.availability;
    const HWND previous_list_view_window = state_.explorer.desktop_list_view_window;
    if (!explorer_watcher_.Start(message_window_.Handle(), state_.explorer, state_.timers))
    {
        Logger::Error(kBootstrapPhase, L"Failed to start the Explorer readiness watcher.");
        return false;
    }

    HandleExplorerStateUpdate(previous_availability, previous_list_view_window);
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
            BeginShutdown(EXIT_FAILURE, L"Shutting down because the message loop encountered a fatal GetMessageW error.");
            return EXIT_FAILURE;
        }

        if (result == 0)
        {
            Logger::Info(kShutdownPhase, L"Message loop exited. exit_code=" + std::to_wstring(static_cast<int>(message.wParam)));
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
    explorer_watcher_.Stop(state_.timers);
    DisarmTimer(TimerId::FirstAutoMove);
    DisarmTimer(TimerId::DisplayChangeDebounce);
}

bool App::ExecuteDesktopIconMove(MoveTrigger trigger)
{
    std::wstring start_message = L"Dispatching desktop icon move from the application coordinator. trigger=\"";
    start_message += MoveTriggerName(trigger);
    start_message += L"\" configured_target_monitor_id=\"";
    start_message += state_.config.target_monitor_id;
    start_message += L"\"";
    Logger::Info(CurrentPhase(state_), start_message);

    MoveResult move_result;
    const bool succeeded = DesktopIconMover::Execute(trigger, state_.config.target_monitor_id, move_result);
    state_.last_move_result = move_result;

    if (move_result.outcome == MoveOutcome::Success)
    {
        state_.last_execution_time = move_result.executed_at;
        if (!ConfigManager::SetLastSuccessfulMoveTime(state_.config, move_result.executed_at))
        {
            Logger::Warning(kRuntimePhase, L"Desktop icon move succeeded, but persisting the last successful move time failed.");
        }
    }

    std::wstring finish_message = L"Application coordinator completed the desktop icon move. trigger=\"";
    finish_message += MoveTriggerName(trigger);
    finish_message += L"\" succeeded=\"";
    finish_message += BoolName(succeeded);
    finish_message += L"\" outcome=\"";
    finish_message += MoveOutcomeName(move_result.outcome);
    finish_message += L"\" skip_reason=\"";
    finish_message += MoveSkipReasonName(move_result.skip_reason);
    finish_message += L"\" target_monitor_id=\"";
    finish_message += move_result.target_monitor_id;
    finish_message += L"\" icon_count=";
    finish_message += std::to_wstring(move_result.icon_count);
    finish_message += L" last_error=";
    finish_message += std::to_wstring(move_result.last_error);

    if (move_result.outcome == MoveOutcome::Success)
    {
        Logger::Info(CurrentPhase(state_), finish_message);
    }
    else if (move_result.outcome == MoveOutcome::Skipped)
    {
        Logger::Warning(CurrentPhase(state_), finish_message);
    }
    else
    {
        Logger::Error(CurrentPhase(state_), finish_message);
    }

    return succeeded;
}

bool App::RefreshMonitorStateForDisplayChange()
{
    MonitorTopology topology;
    if (!MonitorManager::RefreshTopology(topology))
    {
        state_.monitor_topology = {};
        state_.target_monitor = {};
        state_.target_monitor.requested_monitor_id = state_.config.target_monitor_id;
        Logger::Warning(
            kRuntimePhase,
            L"Display change processing could not refresh the monitor topology; skipping the pending automatic rearrange.");
        return false;
    }

    const std::wstring previous_resolved_monitor_id = state_.target_monitor.resolved_monitor_id;
    const TargetMonitorResolution previous_resolution = state_.target_monitor.resolution;

    state_.monitor_topology = std::move(topology);
    state_.target_monitor = MonitorManager::ResolveTargetMonitor(state_.monitor_topology, state_.config.target_monitor_id);

    std::wstring message = L"Display change monitor state refreshed. requested_monitor_id=\"";
    message += state_.target_monitor.requested_monitor_id;
    message += L"\" resolved_monitor_id=\"";
    message += state_.target_monitor.resolved_monitor_id;
    message += L"\" resolution=\"";
    message += TargetMonitorResolutionName(state_.target_monitor.resolution);
    message += L"\" previous_resolved_monitor_id=\"";
    message += previous_resolved_monitor_id;
    message += L"\" previous_resolution=\"";
    message += TargetMonitorResolutionName(previous_resolution);
    message += L"\" monitor_count=";
    message += std::to_wstring(static_cast<unsigned long long>(state_.monitor_topology.monitors.size()));
    Logger::Info(kRuntimePhase, message);
    return true;
}

bool App::RefreshExplorerState(std::wstring_view reason, bool allow_immediate_display_change_move)
{
    const bool previous_suppression_state = suppress_immediate_display_change_move_;
    if (!allow_immediate_display_change_move)
    {
        suppress_immediate_display_change_move_ = true;
    }

    const ExplorerAvailability previous_availability = state_.explorer.availability;
    const HWND previous_list_view_window = state_.explorer.desktop_list_view_window;
    if (!explorer_watcher_.Start(message_window_.Handle(), state_.explorer, state_.timers))
    {
        std::wstring message = L"Failed to refresh Explorer readiness while processing reason=\"";
        message.append(reason.data(), reason.size());
        message += L"\"";
        Logger::Warning(kRuntimePhase, message);
        suppress_immediate_display_change_move_ = previous_suppression_state;
        return false;
    }

    HandleExplorerStateUpdate(previous_availability, previous_list_view_window);
    suppress_immediate_display_change_move_ = previous_suppression_state;
    return true;
}

void App::StartDisplayChangeMoveSession()
{
    ++display_change_generation_;
    display_change_move_session_active_ = true;
    pending_display_change_move_ = true;
    display_change_started_tick_ = ::GetTickCount64();
    display_change_move_attempt_count_ = 0;
}

void App::ClearDisplayChangeMoveSession(std::wstring_view reason, bool warning)
{
    if (!display_change_move_session_active_ && !pending_display_change_move_)
    {
        return;
    }

    const ULONGLONG elapsed_ms =
        display_change_started_tick_ == 0 ? 0 : (::GetTickCount64() - display_change_started_tick_);

    std::wstring message = L"Display change rearrange session closed. generation=";
    message += std::to_wstring(display_change_generation_);
    message += L" attempts=";
    message += std::to_wstring(display_change_move_attempt_count_);
    message += L" elapsed_ms=";
    message += std::to_wstring(elapsed_ms);
    message += L" reason=\"";
    message.append(reason.data(), reason.size());
    message += L"\"";

    if (warning)
    {
        Logger::Warning(kRuntimePhase, message);
    }
    else
    {
        Logger::Info(kRuntimePhase, message);
    }

    display_change_move_session_active_ = false;
    pending_display_change_move_ = false;
    display_change_started_tick_ = 0;
    display_change_move_attempt_count_ = 0;
}

bool App::EnsureDisplayChangeMoveSessionAlive(std::wstring_view reason)
{
    if (!display_change_move_session_active_)
    {
        return false;
    }

    const ULONGLONG elapsed_ms =
        display_change_started_tick_ == 0 ? 0 : (::GetTickCount64() - display_change_started_tick_);
    if (elapsed_ms < kDisplayChangeMoveTimeoutMs)
    {
        return true;
    }

    std::wstring message = L"Stopping automatic desktop icon rearrange after the display change because the session timed out. generation=";
    message += std::to_wstring(display_change_generation_);
    message += L" attempts=";
    message += std::to_wstring(display_change_move_attempt_count_);
    message += L" elapsed_ms=";
    message += std::to_wstring(elapsed_ms);
    message += L" timeout_ms=";
    message += std::to_wstring(kDisplayChangeMoveTimeoutMs);
    message += L" reason=\"";
    message.append(reason.data(), reason.size());
    message += L"\"";
    Logger::Warning(kRuntimePhase, message);

    display_change_move_session_active_ = false;
    pending_display_change_move_ = false;
    display_change_started_tick_ = 0;
    display_change_move_attempt_count_ = 0;
    return false;
}

bool App::ScheduleFirstAutoMove(UINT interval_ms, std::wstring_view reason)
{
    if (state_.first_auto_move_state == FirstAutoMoveState::Completed)
    {
        return true;
    }

    if (state_.timers.first_auto_move_armed)
    {
        return true;
    }

    const FirstAutoMoveState previous_state = state_.first_auto_move_state;
    state_.first_auto_move_state = FirstAutoMoveState::ReadyToRun;

    if (!ArmTimer(TimerId::FirstAutoMove, interval_ms))
    {
        state_.first_auto_move_state = FirstAutoMoveState::Failed;
        return false;
    }

    std::wstring message = L"Startup automatic desktop icon move scheduled. previous_state=\"";
    message += FirstAutoMoveStateName(previous_state);
    message += L"\" delay_ms=";
    message += std::to_wstring(interval_ms);
    message += L" reason=\"";
    message.append(reason.data(), reason.size());
    message += L"\"";
    Logger::Info(CurrentPhase(state_), message);
    return true;
}

void App::HandleTimer(UINT_PTR timer_id)
{
    const ExplorerAvailability previous_availability = state_.explorer.availability;
    const HWND previous_list_view_window = state_.explorer.desktop_list_view_window;

    if (explorer_watcher_.HandleTimer(timer_id, state_.explorer, state_.timers))
    {
        HandleExplorerStateUpdate(previous_availability, previous_list_view_window);
        return;
    }

    const auto managed_timer_id = static_cast<TimerId>(timer_id);

    switch (managed_timer_id)
    {
    case TimerId::ExplorerRetry:
    {
        DisarmTimer(managed_timer_id);

        std::wstring message = L"Managed timer elapsed. timer=\"";
        message += TimerName(managed_timer_id);
        message += L"\"";
        Logger::Info(kRuntimePhase, message);
        return;
    }

    case TimerId::FirstAutoMove:
    {
        HandleFirstAutoMoveTimer();
        return;
    }

    case TimerId::DisplayChangeDebounce:
    {
        DisarmTimer(managed_timer_id);

        std::wstring message = L"Managed timer elapsed. timer=\"";
        message += TimerName(managed_timer_id);
        message += L"\"";
        Logger::Info(kRuntimePhase, message);
        HandleDisplayChangeDebounceTimer();
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

void App::HandleFirstAutoMoveTimer()
{
    DisarmTimer(TimerId::FirstAutoMove);

    std::wstring message = L"Managed timer elapsed. timer=\"";
    message += TimerName(TimerId::FirstAutoMove);
    message += L"\"";
    Logger::Info(CurrentPhase(state_), message);

    if (state_.first_auto_move_state == FirstAutoMoveState::Completed)
    {
        Logger::Info(CurrentPhase(state_), L"Ignoring the startup automatic move timer because the first automatic move already completed.");
        return;
    }

    if (!IsExplorerReady(state_))
    {
        Logger::Warning(
            CurrentPhase(state_),
            L"Startup automatic move timer fired before Explorer was ready; resuming Explorer readiness wait.");

        state_.first_auto_move_state = FirstAutoMoveState::WaitingForExplorer;

        const ExplorerAvailability previous_availability = state_.explorer.availability;
        const HWND previous_list_view_window = state_.explorer.desktop_list_view_window;
        if (!explorer_watcher_.Start(message_window_.Handle(), state_.explorer, state_.timers))
        {
            Logger::Warning(CurrentPhase(state_), L"Failed to restart the Explorer readiness watcher after the startup automatic move timer.");
            state_.first_auto_move_state = FirstAutoMoveState::Failed;
            static_cast<void>(ScheduleFirstAutoMove(kExplorerRetryIntervalMs, L"explorer_wait_restart_failed"));
            return;
        }

        HandleExplorerStateUpdate(previous_availability, previous_list_view_window);
        return;
    }

    state_.first_auto_move_state = FirstAutoMoveState::Running;
    Logger::Info(CurrentPhase(state_), L"Executing the startup automatic desktop icon move.");

    static_cast<void>(ExecuteDesktopIconMove(MoveTrigger::StartupAuto));

    if (!state_.last_move_result.has_value())
    {
        state_.first_auto_move_state = FirstAutoMoveState::Failed;
        Logger::Warning(CurrentPhase(state_), L"Startup automatic desktop icon move did not produce a result; scheduling retry.");
        static_cast<void>(ScheduleFirstAutoMove(kExplorerRetryIntervalMs, L"missing_move_result"));
        return;
    }

    const MoveResult& move_result = *state_.last_move_result;
    std::wstring result_message = L"Startup automatic desktop icon move finished. outcome=\"";
    result_message += MoveOutcomeName(move_result.outcome);
    result_message += L"\" target_monitor_id=\"";
    result_message += move_result.target_monitor_id;
    result_message += L"\" icon_count=";
    result_message += std::to_wstring(move_result.icon_count);
    result_message += L" last_error=";
    result_message += std::to_wstring(move_result.last_error);

    if (move_result.outcome == MoveOutcome::Failed)
    {
        state_.first_auto_move_state = FirstAutoMoveState::Failed;
        Logger::Warning(CurrentPhase(state_), result_message + L"; scheduling retry.");
        static_cast<void>(ScheduleFirstAutoMove(kExplorerRetryIntervalMs, L"previous_attempt_failed"));
        return;
    }

    state_.first_auto_move_state = FirstAutoMoveState::Completed;

    if (display_change_move_session_active_)
    {
        ClearDisplayChangeMoveSession(L"startup automatic move already executed against the current topology", false);
    }

    Logger::Info(CurrentPhase(state_), result_message);
}

void App::HandleDisplayChange(UINT bits_per_pixel, int width, int height)
{
    const bool replacing_session = display_change_move_session_active_;
    const ULONGLONG previous_generation = display_change_generation_;

    StartDisplayChangeMoveSession();

    std::wstring message = L"WM_DISPLAYCHANGE received. generation=";
    message += std::to_wstring(display_change_generation_);
    message += L" bits_per_pixel=";
    message += std::to_wstring(bits_per_pixel);
    message += L" width=";
    message += std::to_wstring(width);
    message += L" height=";
    message += std::to_wstring(height);
    if (replacing_session)
    {
        message += L" superseded_generation=";
        message += std::to_wstring(previous_generation);
    }
    Logger::Info(kRuntimePhase, message);

    if (!ArmTimer(TimerId::DisplayChangeDebounce, kDisplayChangeDebounceMs))
    {
        ClearDisplayChangeMoveSession(L"display change debounce timer could not be armed", true);
    }
}

void App::HandleDisplayChangeDebounceTimer()
{
    if (!EnsureDisplayChangeMoveSessionAlive(L"display_change_debounce_elapsed"))
    {
        return;
    }

    if (!RefreshMonitorStateForDisplayChange())
    {
        ClearDisplayChangeMoveSession(L"monitor topology refresh failed after WM_DISPLAYCHANGE", true);
        return;
    }

    if (state_.target_monitor.resolution == TargetMonitorResolution::Unavailable)
    {
        ClearDisplayChangeMoveSession(L"no eligible secondary monitor is available after WM_DISPLAYCHANGE", true);
        return;
    }

    pending_display_change_move_ = true;

    if (!RefreshExplorerState(L"display_change_debounce", false))
    {
        ClearDisplayChangeMoveSession(L"Explorer readiness refresh failed after WM_DISPLAYCHANGE", true);
        return;
    }

    if (!IsExplorerReady(state_))
    {
        std::wstring message = L"Explorer desktop is not stable after the display change; deferring automatic rearrange until SysListView32 is ready. generation=";
        message += std::to_wstring(display_change_generation_);
        Logger::Info(kRuntimePhase, message);
        return;
    }

    TryExecutePendingDisplayChangeMove(L"display_change_debounce_elapsed");
}

void App::HandleExplorerStateUpdate(ExplorerAvailability previous_availability, HWND previous_list_view_window)
{
    SyncDesktopViewAccessor(previous_availability, previous_list_view_window);

    if (display_change_move_session_active_)
    {
        static_cast<void>(EnsureDisplayChangeMoveSessionAlive(L"explorer_state_update"));
    }

    if (!IsExplorerReady(state_))
    {
        if (state_.first_auto_move_state != FirstAutoMoveState::Completed && state_.timers.first_auto_move_armed)
        {
            DisarmTimer(TimerId::FirstAutoMove);
            Logger::Warning(
                CurrentPhase(state_),
                L"Explorer became unavailable before the startup automatic move executed; cancelled the pending automatic move.");
        }

        if (state_.first_auto_move_state != FirstAutoMoveState::Completed &&
            state_.first_auto_move_state != FirstAutoMoveState::WaitingForExplorer &&
            state_.first_auto_move_state != FirstAutoMoveState::Running)
        {
            state_.first_auto_move_state = FirstAutoMoveState::WaitingForExplorer;
        }

        return;
    }

    if (state_.first_auto_move_state == FirstAutoMoveState::Completed)
    {
        if (pending_display_change_move_ &&
            !state_.timers.display_change_debounce_armed &&
            !suppress_immediate_display_change_move_)
        {
            TryExecutePendingDisplayChangeMove(L"explorer_ready_after_display_change");
        }

        return;
    }

    if (state_.first_auto_move_state == FirstAutoMoveState::Running || state_.timers.first_auto_move_armed)
    {
        return;
    }

    const bool became_ready = previous_availability != ExplorerAvailability::Ready;
    const bool list_view_changed = previous_list_view_window != state_.explorer.desktop_list_view_window;
    if (!became_ready && !list_view_changed && state_.first_auto_move_state == FirstAutoMoveState::ReadyToRun)
    {
        return;
    }

    std::wstring reason = L"explorer_ready";
    UINT interval_ms = kFirstAutoMoveDelayMs;

    if (state_.first_auto_move_state == FirstAutoMoveState::Failed)
    {
        reason = L"explorer_ready_after_failure";
        interval_ms = kExplorerRetryIntervalMs;
    }
    else if (list_view_changed && !became_ready)
    {
        reason = L"desktop_list_view_changed";
    }

    static_cast<void>(ScheduleFirstAutoMove(interval_ms, reason));
}

void App::TryExecutePendingDisplayChangeMove(std::wstring_view reason)
{
    if (!display_change_move_session_active_ || !pending_display_change_move_ || state_.timers.display_change_debounce_armed)
    {
        return;
    }

    if (state_.first_auto_move_state != FirstAutoMoveState::Completed)
    {
        return;
    }

    if (!IsExplorerReady(state_))
    {
        return;
    }

    if (!EnsureDisplayChangeMoveSessionAlive(reason))
    {
        return;
    }

    if (display_change_move_attempt_count_ >= kMaxDisplayChangeMoveAttempts)
    {
        ClearDisplayChangeMoveSession(L"display change retry budget was exhausted before another move attempt", true);
        return;
    }

    if (!RefreshMonitorStateForDisplayChange())
    {
        ClearDisplayChangeMoveSession(L"monitor topology refresh failed before the pending display change move", true);
        return;
    }

    if (state_.target_monitor.resolution == TargetMonitorResolution::Unavailable)
    {
        ClearDisplayChangeMoveSession(L"target monitor is unavailable before the pending display change move", true);
        return;
    }

    ++display_change_move_attempt_count_;

    std::wstring message = L"Executing the pending automatic desktop icon rearrange after a display change. generation=";
    message += std::to_wstring(display_change_generation_);
    message += L" attempt=";
    message += std::to_wstring(display_change_move_attempt_count_);
    message += L"/";
    message += std::to_wstring(kMaxDisplayChangeMoveAttempts);
    message += L" reason=\"";
    message.append(reason.data(), reason.size());
    message += L"\"";
    Logger::Info(kRuntimePhase, message);

    pending_display_change_move_ = false;
    static_cast<void>(ExecuteDesktopIconMove(MoveTrigger::DisplayChange));

    if (!state_.last_move_result.has_value())
    {
        ClearDisplayChangeMoveSession(L"display change rearrange finished without a move result", true);
        return;
    }

    const MoveResult& move_result = *state_.last_move_result;
    if (ShouldRetryDisplayChangeMove(move_result))
    {
        if (!EnsureDisplayChangeMoveSessionAlive(L"display_change_retry"))
        {
            return;
        }

        if (display_change_move_attempt_count_ >= kMaxDisplayChangeMoveAttempts)
        {
            std::wstring retry_limit_message =
                L"Stopping automatic desktop icon rearrange after the display change because the retry budget was exhausted. generation=";
            retry_limit_message += std::to_wstring(display_change_generation_);
            retry_limit_message += L" attempts=";
            retry_limit_message += std::to_wstring(display_change_move_attempt_count_);
            retry_limit_message += L" last_error=";
            retry_limit_message += std::to_wstring(move_result.last_error);
            Logger::Warning(kRuntimePhase, retry_limit_message);
            ClearDisplayChangeMoveSession(L"display change retry budget exhausted after transient failure", true);
            return;
        }

        pending_display_change_move_ = true;

        std::wstring retry_message = L"Display change rearrange hit Explorer instability; waiting before retry. generation=";
        retry_message += std::to_wstring(display_change_generation_);
        retry_message += L" attempts=";
        retry_message += std::to_wstring(display_change_move_attempt_count_);
        retry_message += L" skip_reason=\"";
        retry_message += MoveSkipReasonName(move_result.skip_reason);
        retry_message += L"\" last_error=";
        retry_message += std::to_wstring(move_result.last_error);
        Logger::Warning(kRuntimePhase, retry_message);

        if (!RefreshExplorerState(L"display_change_retry", false))
        {
            ClearDisplayChangeMoveSession(L"Explorer readiness refresh failed while scheduling a display change retry", true);
            return;
        }

        if (IsExplorerReady(state_))
        {
            if (!ArmTimer(TimerId::DisplayChangeDebounce, kExplorerRetryIntervalMs))
            {
                ClearDisplayChangeMoveSession(L"delayed display change retry timer could not be armed", true);
                return;
            }

            std::wstring delayed_retry_message =
                L"Scheduled a delayed display change retry because Explorer was still rebuilding the desktop view. generation=";
            delayed_retry_message += std::to_wstring(display_change_generation_);
            delayed_retry_message += L" next_attempt=";
            delayed_retry_message += std::to_wstring(display_change_move_attempt_count_ + 1);
            Logger::Info(kRuntimePhase, delayed_retry_message);
        }

        return;
    }

    if (move_result.outcome == MoveOutcome::Failed)
    {
        std::wstring failure_message = L"Display change rearrange failed and will not be retried. skip_reason=\"";
        failure_message += MoveSkipReasonName(move_result.skip_reason);
        failure_message += L"\" last_error=";
        failure_message += std::to_wstring(move_result.last_error);
        Logger::Warning(kRuntimePhase, failure_message);
        ClearDisplayChangeMoveSession(L"display change rearrange failed without a retryable Explorer state", true);
        return;
    }

    ClearDisplayChangeMoveSession(L"display change rearrange completed", false);
}

bool App::RefreshDesktopViewContext()
{
    const wchar_t* phase = CurrentPhase(state_);
    const HWND desktop_list_view_window = state_.explorer.desktop_list_view_window;

    if (desktop_list_view_window == nullptr)
    {
        desktop_view_accessor_.Reset();
        return false;
    }

    if (!desktop_view_accessor_.Refresh(desktop_list_view_window))
    {
        Logger::Warning(phase, L"Failed to refresh the desktop shell view accessor for the current Explorer desktop.");
        return false;
    }

    int icon_count = 0;
    if (!desktop_view_accessor_.GetIconCount(icon_count))
    {
        desktop_view_accessor_.Reset();
        Logger::Warning(phase, L"Desktop shell view refresh succeeded, but reading the desktop icon count failed.");
        return false;
    }

    std::wstring message = L"Desktop view accessor refreshed. desktop_list_view=";
    message += std::to_wstring(reinterpret_cast<UINT_PTR>(desktop_list_view_window));
    message += L" icon_count=";
    message += std::to_wstring(icon_count);
    Logger::Info(phase, message);
    return true;
}

void App::SyncDesktopViewAccessor(ExplorerAvailability previous_availability, HWND previous_list_view_window)
{
    const bool explorer_ready = state_.explorer.availability == ExplorerAvailability::Ready &&
                                state_.explorer.desktop_list_view_window != nullptr;

    if (!explorer_ready)
    {
        if (previous_availability == ExplorerAvailability::Ready || previous_list_view_window != nullptr ||
            desktop_view_accessor_.IsReady())
        {
            desktop_view_accessor_.Reset();
        }

        return;
    }

    const bool became_ready = previous_availability != ExplorerAvailability::Ready;
    const bool list_view_changed = previous_list_view_window != state_.explorer.desktop_list_view_window;
    if (!became_ready && !list_view_changed && desktop_view_accessor_.IsReady())
    {
        return;
    }

    static_cast<void>(RefreshDesktopViewContext());
}

void App::BeginShutdown(int exit_code, std::wstring_view reason)
{
    if (shutdown_requested_)
    {
        return;
    }

    std::wstring message = L"Shutdown sequence begins. exit_code=";
    message += std::to_wstring(exit_code);
    message += L" reason=\"";
    message.append(reason.data(), reason.size());
    message += L"\" first_auto_move_state=\"";
    message += FirstAutoMoveStateName(state_.first_auto_move_state);
    message += L"\" display_change_session_active=\"";
    message += BoolName(display_change_move_session_active_);
    message += L"\" pending_display_change_move=\"";
    message += BoolName(pending_display_change_move_);
    message += L"\" explorer_retry_armed=\"";
    message += BoolName(state_.timers.explorer_retry_armed);
    message += L"\" first_auto_move_armed=\"";
    message += BoolName(state_.timers.first_auto_move_armed);
    message += L"\" display_change_debounce_armed=\"";
    message += BoolName(state_.timers.display_change_debounce_armed);
    message += L"\" message_window_available=\"";
    message += BoolName(message_window_.Handle() != nullptr);
    message += L"\"";
    Logger::Info(kShutdownPhase, message);

    shutdown_requested_ = true;
    exit_code_ = exit_code;
    state_.lifecycle = AppLifecycleState::ShuttingDown;
    display_change_move_session_active_ = false;
    pending_display_change_move_ = false;
    suppress_immediate_display_change_move_ = false;
    display_change_started_tick_ = 0;
    display_change_move_attempt_count_ = 0;
    DisarmAllTimers();
    desktop_view_accessor_.Reset();
    tray_manager_.Shutdown();

    const bool had_window = message_window_.Handle() != nullptr;
    message_window_.Destroy();
    if (!had_window)
    {
        ::PostQuitMessage(exit_code_);
    }
}

std::optional<LRESULT> App::OnMessageWindowMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    static_cast<void>(window);

    if (tray_manager_.IsTaskbarCreatedMessage(message))
    {
        if (!tray_manager_.HandleTaskbarCreated())
        {
            Logger::Warning(kRuntimePhase, L"Failed to restore the tray icon after Explorer recreated the taskbar.");
        }

        return 0;
    }

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

    case WM_COMMAND:
    {
        const std::optional<TrayCommand> tray_command = tray_manager_.HandleCommand(w_param);
        if (!tray_command.has_value())
        {
            return std::nullopt;
        }

        CommandDispatchResult dispatch_result;
        if (!CommandHandler::HandleTrayCommand(*tray_command, state_, kRuntimePhase, dispatch_result))
        {
            return 0;
        }

        if (dispatch_result.should_execute_move)
        {
            static_cast<void>(ExecuteDesktopIconMove(dispatch_result.move_trigger));
        }

        if (dispatch_result.should_exit)
        {
            BeginShutdown(EXIT_SUCCESS, L"Shutdown requested from the tray icon menu.");
        }

        return 0;
    }

    case kTrayIconMessage:
    {
        if (tray_manager_.HandleCallbackMessage(l_param) == TrayCallbackAction::ShowContextMenu)
        {
            static_cast<void>(CommandHandler::RefreshTrayState(state_, kRuntimePhase));

            TrayMenuContext menu_context = {};
            menu_context.monitor_topology = &state_.monitor_topology;
            menu_context.target_monitor = &state_.target_monitor;
            menu_context.auto_start_enabled = state_.auto_start_enabled;
            menu_context.auto_start_status_available = state_.auto_start_status_available;

            if (!tray_manager_.ShowContextMenu(menu_context))
            {
                Logger::Warning(kRuntimePhase, L"Failed to show the tray context menu.");
            }
        }

        return 0;
    }

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

