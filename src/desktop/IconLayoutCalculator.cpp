#include "desktop/IconLayoutCalculator.h"

#include <CommCtrl.h>

#include <algorithm>
#include <string>

#include "common/Logger.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kIconLayoutPhase[] = L"icon_layout";
constexpr int kDefaultGridWidth = 96;
constexpr int kDefaultGridHeight = 96;

bool IsRectValid(const RECT& rect) noexcept
{
    return rect.left < rect.right && rect.top < rect.bottom;
}

int ClampCoordinate(int value, int minimum, int maximum) noexcept
{
    if (maximum < minimum)
    {
        return minimum;
    }

    return std::clamp(value, minimum, maximum);
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

SIZE MakeDefaultGridSpacing() noexcept
{
    SIZE spacing = {};
    spacing.cx = kDefaultGridWidth;
    spacing.cy = kDefaultGridHeight;
    return spacing;
}

SIZE ResolveGridSpacing(HWND desktop_list_view_window, bool& used_list_view_spacing) noexcept
{
    used_list_view_spacing = false;

    SIZE spacing = MakeDefaultGridSpacing();
    if (desktop_list_view_window == nullptr)
    {
        return spacing;
    }

    if (::IsWindow(desktop_list_view_window) == FALSE)
    {
        Logger::Warning(
            kIconLayoutPhase,
            L"Desktop ListView handle is unavailable while resolving item spacing; using conservative defaults.");
        return spacing;
    }

    const LRESULT item_spacing = ::SendMessageW(desktop_list_view_window, LVM_GETITEMSPACING, FALSE, 0);
    const int horizontal_spacing = static_cast<int>(LOWORD(item_spacing));
    const int vertical_spacing = static_cast<int>(HIWORD(item_spacing));
    if (horizontal_spacing <= 0 || vertical_spacing <= 0)
    {
        Logger::Warning(
            kIconLayoutPhase,
            L"LVM_GETITEMSPACING did not return usable desktop spacing; using conservative defaults.");
        return spacing;
    }

    spacing.cx = horizontal_spacing;
    spacing.cy = vertical_spacing;
    used_list_view_spacing = true;
    return spacing;
}
}  // namespace

bool IconLayoutCalculator::Calculate(
    const RECT& target_work_area,
    int icon_count,
    IconLayoutResult& result,
    HWND desktop_list_view_window)
{
    result = {};

    if (!IsRectValid(target_work_area))
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kIconLayoutPhase, L"Target work area is invalid.");
        return false;
    }

    if (icon_count < 0)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kIconLayoutPhase, L"Icon count cannot be negative.");
        return false;
    }

    result.metrics.grid_spacing = ResolveGridSpacing(
        desktop_list_view_window,
        result.metrics.used_list_view_spacing);

    if (icon_count == 0)
    {
        Logger::Info(
            kIconLayoutPhase,
            L"Calculated an empty icon layout. target_work_area=" + RectToString(target_work_area));
        return true;
    }

    const int target_left = static_cast<int>(target_work_area.left);
    const int target_top = static_cast<int>(target_work_area.top);
    const int target_right = static_cast<int>(target_work_area.right);
    const int target_bottom = static_cast<int>(target_work_area.bottom);
    const int step_x = std::max(1, static_cast<int>(result.metrics.grid_spacing.cx));
    const int step_y = std::max(1, static_cast<int>(result.metrics.grid_spacing.cy));
    const int max_anchor_x = std::max(target_left, target_right - step_x);
    const int max_anchor_y = std::max(target_top, target_bottom - step_y);

    result.metrics.icons_per_column = std::max(1, ((max_anchor_y - target_top) / step_y) + 1);
    result.metrics.column_count = ((icon_count - 1) / result.metrics.icons_per_column) + 1;

    result.positions.reserve(static_cast<std::size_t>(icon_count));

    bool coordinates_clamped = false;
    for (int icon_index = 0; icon_index < icon_count; ++icon_index)
    {
        const int column_index = icon_index / result.metrics.icons_per_column;
        const int row_index = icon_index % result.metrics.icons_per_column;

        const int raw_x = target_left + (column_index * step_x);
        const int raw_y = target_top + (row_index * step_y);

        POINT position = {};
        position.x = ClampCoordinate(raw_x, target_left, max_anchor_x);
        position.y = ClampCoordinate(raw_y, target_top, max_anchor_y);

        coordinates_clamped = coordinates_clamped || position.x != raw_x || position.y != raw_y;
        result.positions.push_back(position);
    }

    result.metrics.coordinates_clamped = coordinates_clamped;

    std::wstring message = L"Calculated icon layout. target_work_area=";
    message += RectToString(target_work_area);
    message += L" icon_count=";
    message += std::to_wstring(icon_count);
    message += L" grid_spacing=[";
    message += std::to_wstring(result.metrics.grid_spacing.cx);
    message += L",";
    message += std::to_wstring(result.metrics.grid_spacing.cy);
    message += L"] icons_per_column=";
    message += std::to_wstring(result.metrics.icons_per_column);
    message += L" column_count=";
    message += std::to_wstring(result.metrics.column_count);
    message += L" used_list_view_spacing=";
    message += result.metrics.used_list_view_spacing ? L"true" : L"false";
    message += L" coordinates_clamped=";
    message += result.metrics.coordinates_clamped ? L"true" : L"false";

    if (coordinates_clamped)
    {
        Logger::Warning(kIconLayoutPhase, message);
    }
    else
    {
        Logger::Info(kIconLayoutPhase, message);
    }

    return true;
}
}  // namespace icon_mover
