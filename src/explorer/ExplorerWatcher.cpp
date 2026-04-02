#include "explorer/ExplorerWatcher.h"

#include <string>
#include <string_view>

#include "common/Logger.h"
#include "core/Constants.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kExplorerPhase[] = L"explorer";
constexpr wchar_t kProgmanClassName[] = L"Progman";
constexpr wchar_t kWorkerWindowClassName[] = L"WorkerW";
constexpr wchar_t kDesktopHostClassName[] = L"SHELLDLL_DefView";
constexpr wchar_t kDesktopListViewClassName[] = L"SysListView32";
constexpr wchar_t kDesktopListViewWindowName[] = L"FolderView";

constexpr ULONGLONG kStartupWaitTimeoutMs = 30000;
constexpr ULONGLONG kWaitingLogIntervalMs = 5000;

std::wstring HandleToString(HWND window)
{
    return std::to_wstring(reinterpret_cast<UINT_PTR>(window));
}

const wchar_t* ToString(ExplorerAvailability availability) noexcept
{
    switch (availability)
    {
    case ExplorerAvailability::Unknown:
        return L"unknown";

    case ExplorerAvailability::WaitingForProgman:
        return L"waiting_for_progman";

    case ExplorerAvailability::WaitingForDesktopHost:
        return L"waiting_for_desktop_host";

    case ExplorerAvailability::WaitingForListView:
        return L"waiting_for_list_view";

    case ExplorerAvailability::Ready:
        return L"ready";

    case ExplorerAvailability::Lost:
        return L"lost";

    default:
        return L"unknown";
    }
}

bool WindowClassEquals(HWND window, std::wstring_view expected_class) noexcept
{
    if (window == nullptr || expected_class.empty())
    {
        return false;
    }

    wchar_t class_name[64] = {};
    const int copied = ::GetClassNameW(window, class_name, static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
    if (copied <= 0)
    {
        return false;
    }

    return ::CompareStringOrdinal(
               class_name,
               copied,
               expected_class.data(),
               static_cast<int>(expected_class.size()),
               TRUE) == CSTR_EQUAL;
}

HWND FindDesktopHostWindow(HWND progman_window) noexcept
{
    if (progman_window != nullptr)
    {
        const HWND direct_host = ::FindWindowExW(progman_window, nullptr, kDesktopHostClassName, nullptr);
        if (direct_host != nullptr)
        {
            return direct_host;
        }
    }

    // Some Explorer layouts parent SHELLDLL_DefView under a WorkerW instead of Progman.
    HWND worker_window = nullptr;
    while ((worker_window = ::FindWindowExW(nullptr, worker_window, kWorkerWindowClassName, nullptr)) != nullptr)
    {
        const HWND host_window = ::FindWindowExW(worker_window, nullptr, kDesktopHostClassName, nullptr);
        if (host_window != nullptr)
        {
            return host_window;
        }
    }

    return nullptr;
}

HWND FindDesktopListViewWindow(HWND desktop_host_window) noexcept
{
    if (desktop_host_window == nullptr)
    {
        return nullptr;
    }

    HWND list_view_window = ::FindWindowExW(
        desktop_host_window,
        nullptr,
        kDesktopListViewClassName,
        kDesktopListViewWindowName);

    if (list_view_window == nullptr)
    {
        list_view_window = ::FindWindowExW(desktop_host_window, nullptr, kDesktopListViewClassName, nullptr);
    }

    return list_view_window;
}

std::wstring BuildWaitingLogMessage(
    const ExplorerWindowState& explorer_state,
    ULONGLONG elapsed_ms,
    bool background_waiting)
{
    std::wstring message = L"Explorer desktop is not ready yet. availability=\"";
    message += ToString(explorer_state.availability);
    message += L"\" attempts=";
    message += std::to_wstring(static_cast<unsigned long long>(explorer_state.discovery_attempts));
    message += L" elapsed_ms=";
    message += std::to_wstring(elapsed_ms);
    message += L" wait_mode=\"";
    message += background_waiting ? L"background" : L"startup";
    message += L"\" progman=";
    message += HandleToString(explorer_state.progman_window);
    message += L" desktop_host=";
    message += HandleToString(explorer_state.desktop_host_window);
    message += L" desktop_list_view=";
    message += HandleToString(explorer_state.desktop_list_view_window);
    return message;
}

std::wstring BuildReadyLogMessage(const ExplorerWindowState& explorer_state, ULONGLONG elapsed_ms)
{
    std::wstring message = L"Explorer desktop handles are ready. attempts=";
    message += std::to_wstring(static_cast<unsigned long long>(explorer_state.discovery_attempts));
    message += L" elapsed_ms=";
    message += std::to_wstring(elapsed_ms);
    message += L" progman=";
    message += HandleToString(explorer_state.progman_window);
    message += L" desktop_host=";
    message += HandleToString(explorer_state.desktop_host_window);
    message += L" desktop_list_view=";
    message += HandleToString(explorer_state.desktop_list_view_window);
    return message;
}

std::wstring BuildHandleLossLogMessage(const ExplorerWindowState& explorer_state)
{
    std::wstring message = L"Explorer desktop handles became invalid; restarting discovery. progman=";
    message += HandleToString(explorer_state.progman_window);
    message += L" desktop_host=";
    message += HandleToString(explorer_state.desktop_host_window);
    message += L" desktop_list_view=";
    message += HandleToString(explorer_state.desktop_list_view_window);
    return message;
}
}  // namespace

ExplorerWatcher::DiscoveryResult ExplorerWatcher::DiscoverExplorerWindows() noexcept
{
    DiscoveryResult result = {};
    result.progman_window = ::FindWindowW(kProgmanClassName, nullptr);
    if (result.progman_window == nullptr)
    {
        result.availability = ExplorerAvailability::WaitingForProgman;
        return result;
    }

    result.desktop_host_window = FindDesktopHostWindow(result.progman_window);
    if (result.desktop_host_window == nullptr)
    {
        result.availability = ExplorerAvailability::WaitingForDesktopHost;
        return result;
    }

    result.desktop_list_view_window = FindDesktopListViewWindow(result.desktop_host_window);
    if (result.desktop_list_view_window == nullptr)
    {
        result.availability = ExplorerAvailability::WaitingForListView;
        return result;
    }

    result.availability = ExplorerAvailability::Ready;
    return result;
}

bool ExplorerWatcher::IsWaitingAvailability(ExplorerAvailability availability) noexcept
{
    switch (availability)
    {
    case ExplorerAvailability::WaitingForProgman:
    case ExplorerAvailability::WaitingForDesktopHost:
    case ExplorerAvailability::WaitingForListView:
    case ExplorerAvailability::Lost:
        return true;

    case ExplorerAvailability::Unknown:
    case ExplorerAvailability::Ready:
    default:
        return false;
    }
}

bool ExplorerWatcher::Start(HWND message_window, ExplorerWindowState& explorer_state, TimerState& timer_state)
{
    const HWND desktop_list_view_window = GetDesktopListViewWindow(message_window, explorer_state, timer_state);
    return desktop_list_view_window != nullptr || timer_state.explorer_retry_armed;
}

void ExplorerWatcher::Stop(TimerState& timer_state) noexcept
{
    CancelRetryTimer(timer_state);
    owner_window_ = nullptr;
    ResetWaitTracking();
}

bool ExplorerWatcher::HandleTimer(UINT_PTR timer_id, ExplorerWindowState& explorer_state, TimerState& timer_state)
{
    if (timer_id != static_cast<UINT_PTR>(TimerId::ExplorerRetry))
    {
        return false;
    }

    if (owner_window_ == nullptr || ::IsWindow(owner_window_) == FALSE)
    {
        timer_state.explorer_retry_armed = false;
        Logger::Warning(kExplorerPhase, L"Explorer retry timer fired without a valid message window; stopping background wait.");
        return true;
    }

    if (HasValidReadyHandles(explorer_state))
    {
        CancelRetryTimer(timer_state);
        return true;
    }

    static_cast<void>(RefreshDesktopListViewWindow(explorer_state, timer_state));
    return true;
}

HWND ExplorerWatcher::GetDesktopListViewWindow(
    HWND message_window,
    ExplorerWindowState& explorer_state,
    TimerState& timer_state)
{
    if (!EnsureOwnerWindow(message_window))
    {
        Logger::LogLastError(LogLevel::Error, kExplorerPhase, L"Cannot start Explorer discovery because the message window is unavailable.");
        return nullptr;
    }

    if (HasValidReadyHandles(explorer_state))
    {
        CancelRetryTimer(timer_state);
        return explorer_state.desktop_list_view_window;
    }

    if (explorer_state.availability == ExplorerAvailability::Ready)
    {
        NoteHandleLoss(explorer_state);
    }

    return RefreshDesktopListViewWindow(explorer_state, timer_state);
}

bool ExplorerWatcher::EnsureOwnerWindow(HWND message_window)
{
    if (message_window == nullptr || ::IsWindow(message_window) == FALSE)
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }

    owner_window_ = message_window;
    return true;
}

bool ExplorerWatcher::EnsureRetryTimer(TimerState& timer_state)
{
    if (timer_state.explorer_retry_armed)
    {
        return true;
    }

    if (::SetTimer(owner_window_, static_cast<UINT_PTR>(TimerId::ExplorerRetry), kExplorerRetryIntervalMs, nullptr) == 0)
    {
        Logger::LogLastError(LogLevel::Error, kExplorerPhase, L"Failed to arm the Explorer retry timer.");
        return false;
    }

    timer_state.explorer_retry_armed = true;
    return true;
}

void ExplorerWatcher::CancelRetryTimer(TimerState& timer_state) noexcept
{
    timer_state.explorer_retry_armed = false;

    if (owner_window_ != nullptr && ::IsWindow(owner_window_) != FALSE)
    {
        ::KillTimer(owner_window_, static_cast<UINT_PTR>(TimerId::ExplorerRetry));
    }
}

bool ExplorerWatcher::HasValidReadyHandles(const ExplorerWindowState& explorer_state) const noexcept
{
    return explorer_state.availability == ExplorerAvailability::Ready &&
           ::IsWindow(explorer_state.progman_window) != FALSE &&
           ::IsWindow(explorer_state.desktop_host_window) != FALSE &&
           ::IsWindow(explorer_state.desktop_list_view_window) != FALSE &&
           WindowClassEquals(explorer_state.desktop_host_window, kDesktopHostClassName) &&
           WindowClassEquals(explorer_state.desktop_list_view_window, kDesktopListViewClassName);
}

void ExplorerWatcher::ResetWaitTracking() noexcept
{
    wait_started_tick_ = 0;
    last_wait_log_tick_ = 0;
    last_logged_wait_state_ = ExplorerAvailability::Unknown;
    background_wait_logged_ = false;
}

void ExplorerWatcher::NoteHandleLoss(ExplorerWindowState& explorer_state) noexcept
{
    Logger::Warning(kExplorerPhase, BuildHandleLossLogMessage(explorer_state));

    explorer_state.availability = ExplorerAvailability::Lost;
    explorer_state.progman_window = nullptr;
    explorer_state.desktop_host_window = nullptr;
    explorer_state.desktop_list_view_window = nullptr;
    explorer_state.discovery_attempts = 0;
    ResetWaitTracking();
}

HWND ExplorerWatcher::RefreshDesktopListViewWindow(ExplorerWindowState& explorer_state, TimerState& timer_state)
{
    if (wait_started_tick_ == 0)
    {
        wait_started_tick_ = ::GetTickCount64();
        last_wait_log_tick_ = 0;
        last_logged_wait_state_ = ExplorerAvailability::Unknown;
        background_wait_logged_ = false;

        if (!IsWaitingAvailability(explorer_state.availability))
        {
            explorer_state.discovery_attempts = 0;
        }
    }

    ++explorer_state.discovery_attempts;

    const DiscoveryResult discovery = DiscoverExplorerWindows();
    explorer_state.availability = discovery.availability;
    explorer_state.progman_window = discovery.progman_window;
    explorer_state.desktop_host_window = discovery.desktop_host_window;
    explorer_state.desktop_list_view_window = discovery.desktop_list_view_window;

    if (explorer_state.availability == ExplorerAvailability::Ready)
    {
        CancelRetryTimer(timer_state);
        LogReadyState(explorer_state);
        ResetWaitTracking();
        return explorer_state.desktop_list_view_window;
    }

    if (!EnsureRetryTimer(timer_state))
    {
        return nullptr;
    }

    LogWaitingState(explorer_state);
    return nullptr;
}

void ExplorerWatcher::LogWaitingState(const ExplorerWindowState& explorer_state)
{
    const ULONGLONG now = ::GetTickCount64();
    const ULONGLONG elapsed_ms = wait_started_tick_ == 0 ? 0 : (now - wait_started_tick_);
    const bool background_waiting = elapsed_ms >= kStartupWaitTimeoutMs;

    if (background_waiting && !background_wait_logged_)
    {
        Logger::Warning(
            kExplorerPhase,
            L"Explorer desktop did not become ready within the startup wait window; continuing background wait. elapsed_ms=" +
                std::to_wstring(elapsed_ms));
        background_wait_logged_ = true;
        last_wait_log_tick_ = now;
        last_logged_wait_state_ = explorer_state.availability;
        return;
    }

    const bool should_log = last_wait_log_tick_ == 0 || last_logged_wait_state_ != explorer_state.availability ||
                            (now - last_wait_log_tick_) >= kWaitingLogIntervalMs;

    if (!should_log)
    {
        return;
    }

    Logger::Info(kExplorerPhase, BuildWaitingLogMessage(explorer_state, elapsed_ms, background_waiting));
    last_wait_log_tick_ = now;
    last_logged_wait_state_ = explorer_state.availability;
}

void ExplorerWatcher::LogReadyState(const ExplorerWindowState& explorer_state) const
{
    const ULONGLONG elapsed_ms = wait_started_tick_ == 0 ? 0 : (::GetTickCount64() - wait_started_tick_);
    Logger::Info(kExplorerPhase, BuildReadyLogMessage(explorer_state, elapsed_ms));
}
}  // namespace icon_mover
