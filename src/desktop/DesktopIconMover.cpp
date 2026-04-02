#include "desktop/DesktopIconMover.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

#include <CommCtrl.h>

#include "common/Logger.h"
#include "common/Win32Error.h"
#include "desktop/DesktopViewAccessor.h"
#include "desktop/IconLayoutCalculator.h"
#include "monitor/MonitorManager.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kDesktopMovePhase[] = L"desktop_move";
constexpr wchar_t kProgmanClassName[] = L"Progman";
constexpr wchar_t kWorkerWindowClassName[] = L"WorkerW";
constexpr wchar_t kDesktopHostClassName[] = L"SHELLDLL_DefView";
constexpr wchar_t kDesktopListViewClassName[] = L"SysListView32";
constexpr wchar_t kDesktopListViewWindowName[] = L"FolderView";

const wchar_t* ToString(MoveTrigger trigger) noexcept
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

const wchar_t* ToString(MoveOutcome outcome) noexcept
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

const wchar_t* ToString(MoveSkipReason reason) noexcept
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

const wchar_t* ToString(TargetMonitorResolution resolution) noexcept
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

std::wstring RectToString(const RECT& rect)
{
    std::wstring text = L"[";
    text += std::to_wstring(rect.left);
    text += L",";
    text += std::to_wstring(rect.top);
    text += L",";
    text += std::to_wstring(rect.right);
    text += L",";
    text += std::to_wstring(rect.bottom);
    text += L"]";
    return text;
}

std::wstring PointToString(const POINT& point)
{
    std::wstring text = L"[";
    text += std::to_wstring(point.x);
    text += L",";
    text += std::to_wstring(point.y);
    text += L"]";
    return text;
}

std::wstring HandleToString(HWND window)
{
    return std::to_wstring(reinterpret_cast<UINT_PTR>(window));
}

DWORD ResolveErrorCode(DWORD fallback = ERROR_GEN_FAILURE) noexcept
{
    const DWORD error_code = ::GetLastError();
    return error_code != ERROR_SUCCESS ? error_code : fallback;
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

bool PointInRect(const RECT& rect, const POINT& point) noexcept
{
    return ::PtInRect(&rect, point) != FALSE;
}

RECT TranslateScreenRectToViewCoordinates(const RECT& screen_rect, const RECT& virtual_bounds) noexcept
{
    RECT translated_rect = screen_rect;
    ::OffsetRect(&translated_rect, -virtual_bounds.left, -virtual_bounds.top);
    return translated_rect;
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

HWND DiscoverDesktopListViewWindow()
{
    const HWND progman_window = ::FindWindowW(kProgmanClassName, nullptr);
    if (progman_window == nullptr)
    {
        ::SetLastError(ERROR_FILE_NOT_FOUND);
        Logger::LogLastError(LogLevel::Error, kDesktopMovePhase, L"FindWindowW(Progman) did not return the desktop host window.");
        return nullptr;
    }

    const HWND desktop_host_window = FindDesktopHostWindow(progman_window);
    if (desktop_host_window == nullptr)
    {
        ::SetLastError(ERROR_FILE_NOT_FOUND);
        Logger::LogLastError(LogLevel::Error, kDesktopMovePhase, L"FindWindowExW could not locate SHELLDLL_DefView for the desktop.");
        return nullptr;
    }

    const HWND list_view_window = FindDesktopListViewWindow(desktop_host_window);
    if (list_view_window == nullptr || !WindowClassEquals(list_view_window, kDesktopListViewClassName))
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopMovePhase, L"FindWindowExW could not locate the desktop SysListView32.");
        return nullptr;
    }

    Logger::Info(
        kDesktopMovePhase,
        L"Confirmed desktop ListView handle. progman=" + HandleToString(progman_window) +
            L" desktop_host=" + HandleToString(desktop_host_window) +
            L" desktop_list_view=" + HandleToString(list_view_window));
    return list_view_window;
}

std::wstring BuildAttemptLabel(int attempt_index)
{
    return std::to_wstring(attempt_index);
}

void LogAttemptStart(MoveTrigger trigger, std::wstring_view requested_monitor_id, int attempt_index)
{
    std::wstring message = L"Starting desktop icon move attempt. trigger=\"";
    message += ToString(trigger);
    message += L"\" attempt=";
    message += BuildAttemptLabel(attempt_index);
    message += L" requested_monitor_id=\"";
    message.append(requested_monitor_id.data(), requested_monitor_id.size());
    message += L"\"";
    Logger::Info(kDesktopMovePhase, message);
}

void LogTargetMonitorSelection(const MonitorInfo& monitor, const TargetMonitorSelection& selection, int attempt_index)
{
    std::wstring message = L"Resolved target monitor for desktop icon move. attempt=";
    message += BuildAttemptLabel(attempt_index);
    message += L" resolution=\"";
    message += ToString(selection.resolution);
    message += L"\" requested_monitor_id=\"";
    message += selection.requested_monitor_id;
    message += L"\" target_monitor_id=\"";
    message += monitor.monitor_id;
    message += L"\" display_index=";
    message += std::to_wstring(monitor.display_index);
    message += L" work_area=";
    message += RectToString(monitor.work_area_rect);
    Logger::Info(kDesktopMovePhase, message);
}

struct DesktopIconRecord final
{
    int icon_index = -1;
    POINT current_position = {};
    std::wstring item_type_text;
    std::wstring display_name;
    std::wstring parsing_name;
    int sort_priority = 100;
};

struct IconMoveAssignment final
{
    int icon_index = -1;
    POINT position = {};
};

int CompareTextOrdinal(std::wstring_view left, std::wstring_view right) noexcept
{
    const int comparison = ::CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE);

    if (comparison == CSTR_LESS_THAN)
    {
        return -1;
    }

    if (comparison == CSTR_GREATER_THAN)
    {
        return 1;
    }

    return 0;
}

bool ContainsCaseInsensitive(std::wstring_view text, std::wstring_view pattern) noexcept
{
    if (pattern.empty() || text.size() < pattern.size())
    {
        return false;
    }

    const auto equals_ignore_case = [](wchar_t left, wchar_t right) noexcept
    {
        return std::towlower(left) == std::towlower(right);
    };

    const auto it = std::search(text.begin(), text.end(), pattern.begin(), pattern.end(), equals_ignore_case);
    return it != text.end();
}

int ResolveDesktopSortPriority(std::wstring_view parsing_name) noexcept
{
    constexpr wchar_t kThisPcClsid[] = L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    constexpr wchar_t kRecycleBinClsid[] = L"{645FF040-5081-101B-9F08-00AA002F954E}";

    if (ContainsCaseInsensitive(parsing_name, kThisPcClsid))
    {
        return 0;
    }

    if (ContainsCaseInsensitive(parsing_name, kRecycleBinClsid))
    {
        return 1;
    }

    return 100;
}

bool DesktopIconRecordLess(const DesktopIconRecord& left, const DesktopIconRecord& right) noexcept
{
    if (left.sort_priority != right.sort_priority)
    {
        return left.sort_priority < right.sort_priority;
    }

    const int item_type_order = CompareTextOrdinal(left.item_type_text, right.item_type_text);
    if (item_type_order != 0)
    {
        return item_type_order < 0;
    }

    const int display_name_order = CompareTextOrdinal(left.display_name, right.display_name);
    if (display_name_order != 0)
    {
        return display_name_order < 0;
    }

    return left.icon_index < right.icon_index;
}

bool CollectIconRecords(
    DesktopViewAccessor& desktop_view_accessor,
    int icon_count,
    std::vector<DesktopIconRecord>& icons)
{
    icons.clear();
    icons.reserve(static_cast<std::size_t>(icon_count));

    for (int icon_index = 0; icon_index < icon_count; ++icon_index)
    {
        DesktopIconRecord record = {};
        record.icon_index = icon_index;

        if (!desktop_view_accessor.GetIconPosition(icon_index, record.current_position))
        {
            return false;
        }

        if (!desktop_view_accessor.GetItemTypeText(icon_index, record.item_type_text))
        {
            const DWORD item_type_error = ResolveErrorCode();
            Logger::Warning(
                kDesktopMovePhase,
                L"Failed to query a desktop icon item type; falling back to an empty type label for ordering. icon_index=" +
                    std::to_wstring(icon_index) + L" " + DescribeWin32Error(item_type_error));
            record.item_type_text.clear();
        }

        if (!desktop_view_accessor.GetItemDisplayName(icon_index, record.display_name))
        {
            const DWORD display_name_error = ResolveErrorCode();
            Logger::Warning(
                kDesktopMovePhase,
                L"Failed to query a desktop icon display name; falling back to index ordering inside its item type. icon_index=" +
                    std::to_wstring(icon_index) + L" " + DescribeWin32Error(display_name_error));
            record.display_name.clear();
        }

        if (!desktop_view_accessor.GetItemParsingName(icon_index, record.parsing_name))
        {
            const DWORD parsing_name_error = ResolveErrorCode();
            Logger::Warning(
                kDesktopMovePhase,
                L"Failed to query a desktop icon parsing name; falling back to normal item-type ordering. icon_index=" +
                    std::to_wstring(icon_index) + L" " + DescribeWin32Error(parsing_name_error));
            record.parsing_name.clear();
        }

        record.sort_priority = ResolveDesktopSortPriority(record.parsing_name);

        icons.push_back(std::move(record));
    }

    return true;
}

bool BuildFullSortedAssignments(
    const RECT& target_work_area,
    HWND desktop_list_view_window,
    std::vector<DesktopIconRecord> icons,
    std::vector<IconMoveAssignment>& assignments,
    MoveResult& result,
    int attempt_index)
{
    assignments.clear();
    if (icons.empty())
    {
        return true;
    }

    std::vector<POINT> sample_positions;
    sample_positions.reserve(icons.size());
    for (const DesktopIconRecord& icon : icons)
    {
        sample_positions.push_back(icon.current_position);
    }

    std::stable_sort(icons.begin(), icons.end(), DesktopIconRecordLess);

    IconLayoutResult layout;
    if (!IconLayoutCalculator::Calculate(
            target_work_area,
            static_cast<int>(icons.size()),
            layout,
            desktop_list_view_window,
            sample_positions))
    {
        result.last_error = ResolveErrorCode();
        Logger::Error(
            kDesktopMovePhase,
            L"Failed to calculate a fully sorted item-type layout for the desktop icons. attempt=" +
                BuildAttemptLabel(attempt_index) + L" " + DescribeWin32Error(result.last_error));
        return false;
    }

    assignments.reserve(icons.size());
    for (std::size_t index = 0; index < icons.size(); ++index)
    {
        assignments.push_back(IconMoveAssignment{icons[index].icon_index, layout.positions[index]});
    }

    Logger::Info(
        kDesktopMovePhase,
        L"Built a full desktop icon layout ordered by item type from the target monitor top-left corner. attempt=" +
            BuildAttemptLabel(attempt_index) + L" icon_count=" +
            std::to_wstring(static_cast<unsigned long long>(icons.size())));
    return true;
}

void LogFinalResult(const MoveResult& result, int attempt_count)
{
    std::wstring message = L"Desktop icon move finished. trigger=\"";
    message += ToString(result.trigger);
    message += L"\" attempts=";
    message += std::to_wstring(attempt_count);
    message += L" outcome=\"";
    message += ToString(result.outcome);
    message += L"\" skip_reason=\"";
    message += ToString(result.skip_reason);
    message += L"\" target_monitor_id=\"";
    message += result.target_monitor_id;
    message += L"\" icon_count=";
    message += std::to_wstring(result.icon_count);
    message += L" last_error=";
    message += std::to_wstring(result.last_error);

    if (result.outcome == MoveOutcome::Success)
    {
        Logger::Info(kDesktopMovePhase, message);
    }
    else if (result.outcome == MoveOutcome::Skipped)
    {
        Logger::Warning(kDesktopMovePhase, message);
    }
    else
    {
        Logger::Error(kDesktopMovePhase, message);
    }
}

bool ApplyAssignments(
    HWND desktop_list_view_window,
    const std::vector<IconMoveAssignment>& assignments,
    MoveResult& result,
    int attempt_index)
{
    if (assignments.empty())
    {
        return true;
    }

    if (::IsWindow(desktop_list_view_window) == FALSE)
    {
        result.last_error = ERROR_INVALID_WINDOW_HANDLE;
        Logger::Error(
            kDesktopMovePhase,
            L"Desktop ListView handle became invalid before icon positions were applied. attempt=" + BuildAttemptLabel(attempt_index) +
                L" " + DescribeWin32Error(result.last_error));
        return false;
    }

    for (const IconMoveAssignment& assignment : assignments)
    {
        ::SetLastError(ERROR_SUCCESS);
        const BOOL applied = ListView_SetItemPosition(
            desktop_list_view_window,
            assignment.icon_index,
            assignment.position.x,
            assignment.position.y);
        if (applied == FALSE)
        {
            result.last_error = ResolveErrorCode();

            std::wstring message = L"LVM_SETITEMPOSITION failed while moving a desktop icon. attempt=";
            message += BuildAttemptLabel(attempt_index);
            message += L" icon_index=";
            message += std::to_wstring(assignment.icon_index);
            message += L" position=";
            message += PointToString(assignment.position);
            message += L" ";
            message += DescribeWin32Error(result.last_error);
            Logger::Error(kDesktopMovePhase, message);
            return false;
        }
    }

    return true;
}

void LogPostApplyDiagnostics(
    DesktopViewAccessor& desktop_view_accessor,
    const std::vector<IconMoveAssignment>& assignments,
    const RECT& layout_work_area,
    int icon_count,
    int attempt_index)
{
    if (assignments.empty() || icon_count <= 0)
    {
        return;
    }

    POINT requested_min = assignments.front().position;
    for (const IconMoveAssignment& assignment : assignments)
    {
        requested_min.x = std::min(requested_min.x, assignment.position.x);
        requested_min.y = std::min(requested_min.y, assignment.position.y);
    }

    POINT actual_min = {};
    bool initialized = false;
    for (int icon_index = 0; icon_index < icon_count; ++icon_index)
    {
        POINT actual_position = {};
        if (!desktop_view_accessor.GetIconPosition(icon_index, actual_position))
        {
            Logger::Warning(
                kDesktopMovePhase,
                L"Post-apply desktop icon verification could not read an icon position. attempt=" +
                    BuildAttemptLabel(attempt_index) + L" icon_index=" + std::to_wstring(icon_index));
            return;
        }

        if (!initialized)
        {
            actual_min = actual_position;
            initialized = true;
            continue;
        }

        actual_min.x = std::min(actual_min.x, actual_position.x);
        actual_min.y = std::min(actual_min.y, actual_position.y);
    }

    Logger::Info(
        kDesktopMovePhase,
        L"Post-apply desktop icon verification completed. attempt=" + BuildAttemptLabel(attempt_index) +
            L" layout_work_area=" + RectToString(layout_work_area) +
            L" requested_min=" + PointToString(requested_min) +
            L" actual_min=" + PointToString(actual_min));
}

bool ExecuteAttempt(MoveTrigger trigger, std::wstring_view requested_monitor_id, int attempt_index, MoveResult& result)
{
    result = {};
    result.trigger = trigger;

    LogAttemptStart(trigger, requested_monitor_id, attempt_index);

    MonitorTopology topology;
    if (!MonitorManager::RefreshTopology(topology))
    {
        result.outcome = MoveOutcome::Failed;
        result.last_error = ResolveErrorCode();
        Logger::Error(
            kDesktopMovePhase,
            L"Failed to refresh the monitor topology before moving desktop icons. attempt=" + BuildAttemptLabel(attempt_index) +
                L" " + DescribeWin32Error(result.last_error));
        return false;
    }

    const TargetMonitorSelection selection = MonitorManager::ResolveTargetMonitor(topology, requested_monitor_id);
    if (selection.resolution == TargetMonitorResolution::Unavailable)
    {
        result.outcome = MoveOutcome::Skipped;
        result.skip_reason = MoveSkipReason::NoAvailableMonitor;
        result.last_error = ERROR_SUCCESS;
        Logger::Warning(
            kDesktopMovePhase,
            L"Skipping desktop icon move because no eligible secondary monitor is available. attempt=" +
                BuildAttemptLabel(attempt_index));
        return false;
    }

    const MonitorInfo* target_monitor = MonitorManager::FindMonitorById(topology, selection.resolved_monitor_id);
    if (target_monitor == nullptr)
    {
        result.outcome = MoveOutcome::Skipped;
        result.skip_reason = MoveSkipReason::TargetMonitorUnavailable;
        result.last_error = ERROR_NOT_FOUND;
        Logger::Warning(
            kDesktopMovePhase,
            L"Skipping desktop icon move because the resolved target monitor is no longer available. attempt=" +
                BuildAttemptLabel(attempt_index) + L" target_monitor_id=\"" + selection.resolved_monitor_id + L"\"");
        return false;
    }

    result.target_monitor_id = target_monitor->monitor_id;
    LogTargetMonitorSelection(*target_monitor, selection, attempt_index);

    const RECT layout_work_area = TranslateScreenRectToViewCoordinates(
        target_monitor->work_area_rect,
        topology.virtual_screen.bounds);
    Logger::Info(
        kDesktopMovePhase,
        L"Translated target monitor work area into desktop view coordinates. attempt=" + BuildAttemptLabel(attempt_index) +
            L" screen_work_area=" + RectToString(target_monitor->work_area_rect) +
            L" virtual_bounds=" + RectToString(topology.virtual_screen.bounds) +
            L" layout_work_area=" + RectToString(layout_work_area));

    const HWND desktop_list_view_window = DiscoverDesktopListViewWindow();
    if (desktop_list_view_window == nullptr)
    {
        result.outcome = MoveOutcome::Failed;
        result.skip_reason = MoveSkipReason::ExplorerNotReady;
        result.last_error = ResolveErrorCode(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }

    DesktopViewAccessor desktop_view_accessor;
    if (!desktop_view_accessor.Refresh(desktop_list_view_window))
    {
        result.outcome = MoveOutcome::Failed;
        result.last_error = ResolveErrorCode();

        const bool transient_refresh_failure = DesktopViewAccessor::IsTransientRefreshError(result.last_error);
        if (transient_refresh_failure)
        {
            result.skip_reason = MoveSkipReason::ExplorerNotReady;
            Logger::Warning(
                kDesktopMovePhase,
                L"DesktopViewAccessor could not bind to the current desktop ListView because Explorer is still rebuilding. attempt=" +
                    BuildAttemptLabel(attempt_index) + L" desktop_list_view=" + HandleToString(desktop_list_view_window) +
                    L" " + DescribeWin32Error(result.last_error));
        }
        else
        {
            Logger::Error(
                kDesktopMovePhase,
                L"Failed to bind DesktopViewAccessor to the confirmed desktop ListView. attempt=" +
                    BuildAttemptLabel(attempt_index) + L" desktop_list_view=" + HandleToString(desktop_list_view_window) +
                    L" " + DescribeWin32Error(result.last_error));
        }

        return false;
    }

    if (!desktop_view_accessor.GetIconCount(result.icon_count))
    {
        result.outcome = MoveOutcome::Failed;
        result.last_error = ResolveErrorCode();
        Logger::Error(
            kDesktopMovePhase,
            L"Failed to read the desktop icon count before moving icons. attempt=" + BuildAttemptLabel(attempt_index) +
                L" " + DescribeWin32Error(result.last_error));
        return false;
    }

    if (result.icon_count <= 0)
    {
        result.outcome = MoveOutcome::Skipped;
        result.skip_reason = MoveSkipReason::NoDesktopIcons;
        result.last_error = ERROR_SUCCESS;
        Logger::Info(
            kDesktopMovePhase,
            L"Skipping desktop icon move because the desktop contains no icons. attempt=" + BuildAttemptLabel(attempt_index) +
                L" target_monitor_id=\"" + result.target_monitor_id + L"\"");
        return false;
    }

    std::vector<DesktopIconRecord> icons;
    if (!CollectIconRecords(desktop_view_accessor, result.icon_count, icons))
    {
        result.outcome = MoveOutcome::Failed;
        result.last_error = ResolveErrorCode();
        Logger::Error(
            kDesktopMovePhase,
            L"Failed to inspect the current desktop icon positions and metadata before moving icons. attempt=" +
                BuildAttemptLabel(attempt_index) + L" target_monitor_id=\"" + result.target_monitor_id + L"\" " +
                DescribeWin32Error(result.last_error));
        return false;
    }

    std::vector<IconMoveAssignment> assignments;
    if (!BuildFullSortedAssignments(
            layout_work_area,
            desktop_list_view_window,
            std::move(icons),
            assignments,
            result,
            attempt_index))
    {
        result.outcome = MoveOutcome::Failed;
        return false;
    }

    Logger::Info(
        kDesktopMovePhase,
        L"Applying desktop icon layout. attempt=" + BuildAttemptLabel(attempt_index) +
            L" target_monitor_id=\"" + result.target_monitor_id + L"\" icon_count=" +
            std::to_wstring(result.icon_count) + L" moved_icons=" +
            std::to_wstring(static_cast<unsigned long long>(assignments.size())) + L" desktop_list_view=" +
            HandleToString(desktop_list_view_window));

    if (!ApplyAssignments(desktop_list_view_window, assignments, result, attempt_index))
    {
        result.outcome = MoveOutcome::Failed;
        return false;
    }

    LogPostApplyDiagnostics(
        desktop_view_accessor,
        assignments,
        layout_work_area,
        result.icon_count,
        attempt_index);

    result.outcome = MoveOutcome::Success;
    result.skip_reason = MoveSkipReason::None;
    result.executed_at = std::chrono::system_clock::now();
    result.last_error = ERROR_SUCCESS;
    return true;
}
}  // namespace

bool DesktopIconMover::Execute(MoveTrigger trigger, std::wstring_view requested_monitor_id, MoveResult& result)
{
    MoveResult first_attempt;
    if (ExecuteAttempt(trigger, requested_monitor_id, 1, first_attempt))
    {
        result = std::move(first_attempt);
        LogFinalResult(result, 1);
        return true;
    }

    if (first_attempt.outcome != MoveOutcome::Failed)
    {
        result = std::move(first_attempt);
        LogFinalResult(result, 1);
        return false;
    }

    Logger::Warning(
        kDesktopMovePhase,
        L"Desktop icon move attempt failed; re-discovering SysListView32 and retrying once.");

    MoveResult second_attempt;
    const bool succeeded = ExecuteAttempt(trigger, requested_monitor_id, 2, second_attempt);
    result = std::move(second_attempt);
    LogFinalResult(result, 2);
    return succeeded;
}
}  // namespace icon_mover
