#include "desktop/IconLayoutCalculator.h"

#include <CommCtrl.h>

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include "common/Logger.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kIconLayoutPhase[] = L"icon_layout";
constexpr int kDefaultGridWidth = 96;
constexpr int kDefaultGridHeight = 96;
constexpr int kMinimumObservedGridStep = 16;
constexpr int kMaximumObservedGridStep = 2048;

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

int NormalizeModulo(int value, int modulus) noexcept
{
    if (modulus <= 0)
    {
        return 0;
    }

    int remainder = value % modulus;
    if (remainder < 0)
    {
        remainder += modulus;
    }

    return remainder;
}

int AlignToPhaseAtOrAfter(int minimum, int step, int phase) noexcept
{
    if (step <= 0)
    {
        return minimum;
    }

    const int normalized_phase = NormalizeModulo(phase, step);
    const int normalized_minimum = NormalizeModulo(minimum, step);
    const int delta = NormalizeModulo(normalized_phase - normalized_minimum, step);
    return minimum + delta;
}

int AlignToPhaseAtOrBefore(int maximum, int step, int phase) noexcept
{
    if (step <= 0)
    {
        return maximum;
    }

    const int normalized_phase = NormalizeModulo(phase, step);
    const int normalized_maximum = NormalizeModulo(maximum, step);
    const int delta = NormalizeModulo(normalized_maximum - normalized_phase, step);
    return maximum - delta;
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

std::vector<int> ExtractUniqueAxisValues(std::span<const POINT> positions, bool horizontal)
{
    std::vector<int> values;
    values.reserve(positions.size());

    for (const POINT& position : positions)
    {
        values.push_back(horizontal ? position.x : position.y);
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

int ResolveObservedGridStep(const std::vector<int>& axis_values) noexcept
{
    int resolved_step = 0;
    for (std::size_t index = 1; index < axis_values.size(); ++index)
    {
        const int difference = axis_values[index] - axis_values[index - 1];
        if (difference < kMinimumObservedGridStep || difference > kMaximumObservedGridStep)
        {
            continue;
        }

        resolved_step = resolved_step == 0 ? difference : std::gcd(resolved_step, difference);
    }

    if (resolved_step < kMinimumObservedGridStep)
    {
        return 0;
    }

    return resolved_step;
}

int ResolveObservedGridPhase(const std::vector<int>& axis_values, int step) noexcept
{
    if (axis_values.empty() || step <= 0)
    {
        return 0;
    }

    std::vector<int> counts(static_cast<std::size_t>(step), 0);
    for (const int value : axis_values)
    {
        ++counts[static_cast<std::size_t>(NormalizeModulo(value, step))];
    }

    int best_phase = 0;
    int best_count = -1;
    for (int phase = 0; phase < step; ++phase)
    {
        if (counts[static_cast<std::size_t>(phase)] > best_count)
        {
            best_phase = phase;
            best_count = counts[static_cast<std::size_t>(phase)];
        }
    }

    return best_phase;
}
}  // namespace

bool IconLayoutCalculator::Calculate(
    const RECT& target_work_area,
    int icon_count,
    IconLayoutResult& result,
    HWND desktop_list_view_window,
    std::span<const POINT> sample_positions)
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

    const std::vector<int> observed_x_values = ExtractUniqueAxisValues(sample_positions, true);
    const std::vector<int> observed_y_values = ExtractUniqueAxisValues(sample_positions, false);

    const int observed_step_x = ResolveObservedGridStep(observed_x_values);
    const int observed_step_y = ResolveObservedGridStep(observed_y_values);
    if (observed_step_x > 0)
    {
        result.metrics.grid_spacing.cx = observed_step_x;
        result.metrics.used_observed_spacing = true;
    }

    if (observed_step_y > 0)
    {
        result.metrics.grid_spacing.cy = observed_step_y;
        result.metrics.used_observed_spacing = true;
    }

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
    result.metrics.grid_phase.x = ResolveObservedGridPhase(observed_x_values, step_x);
    result.metrics.grid_phase.y = ResolveObservedGridPhase(observed_y_values, step_y);

    // In desktop view coordinates, Windows snaps icons to a stable global grid phase
    // when "Align icons to grid" is enabled. Anchor the requested layout to that
    // phase so Explorer does not shift the first column after we apply positions.
    const int start_x = AlignToPhaseAtOrAfter(target_left, step_x, result.metrics.grid_phase.x);
    const int start_y = AlignToPhaseAtOrAfter(target_top, step_y, result.metrics.grid_phase.y);
    const int max_anchor_x = AlignToPhaseAtOrBefore(
        std::max(target_left, target_right - step_x),
        step_x,
        result.metrics.grid_phase.x);
    const int max_anchor_y = AlignToPhaseAtOrBefore(
        std::max(target_top, target_bottom - step_y),
        step_y,
        result.metrics.grid_phase.y);

    result.metrics.start_position.x = start_x;
    result.metrics.start_position.y = start_y;
    result.metrics.available_columns = start_x <= max_anchor_x ? ((max_anchor_x - start_x) / step_x) + 1 : 0;
    result.metrics.available_rows = start_y <= max_anchor_y ? ((max_anchor_y - start_y) / step_y) + 1 : 0;
    result.metrics.icons_per_column = std::max(1, result.metrics.available_rows);
    result.metrics.column_count = ((icon_count - 1) / result.metrics.icons_per_column) + 1;

    result.positions.reserve(static_cast<std::size_t>(icon_count));

    const long long available_capacity =
        static_cast<long long>(result.metrics.available_columns) * static_cast<long long>(result.metrics.available_rows);
    bool coordinates_clamped = available_capacity < icon_count;
    for (int icon_index = 0; icon_index < icon_count; ++icon_index)
    {
        const int column_index = icon_index / result.metrics.icons_per_column;
        const int row_index = icon_index % result.metrics.icons_per_column;

        const int raw_x = start_x + (column_index * step_x);
        const int raw_y = start_y + (row_index * step_y);

        POINT position = {};
        position.x = ClampCoordinate(raw_x, start_x, max_anchor_x);
        position.y = ClampCoordinate(raw_y, start_y, max_anchor_y);

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
    message += L" used_observed_spacing=";
    message += result.metrics.used_observed_spacing ? L"true" : L"false";
    message += L" grid_phase=";
    message += PointToString(result.metrics.grid_phase);
    message += L" start_position=";
    message += PointToString(result.metrics.start_position);
    message += L" available_rows=";
    message += std::to_wstring(result.metrics.available_rows);
    message += L" available_columns=";
    message += std::to_wstring(result.metrics.available_columns);
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
