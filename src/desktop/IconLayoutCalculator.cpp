#include "desktop/IconLayoutCalculator.h"

#include <CommCtrl.h>

#include <algorithm>
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

int ScaleForDpi(int value, UINT from_dpi, UINT to_dpi) noexcept
{
    if (value <= 0 || from_dpi == 0 || to_dpi == 0 || from_dpi == to_dpi)
    {
        return value;
    }

    return std::max(1, ::MulDiv(value, static_cast<int>(to_dpi), static_cast<int>(from_dpi)));
}

SIZE MakeDefaultGridSpacing() noexcept
{
    SIZE spacing = {};
    spacing.cx = kDefaultGridWidth;
    spacing.cy = kDefaultGridHeight;
    return spacing;
}

SIZE ScaleGridSpacing(const SIZE& raw_spacing, UINT source_dpi, UINT target_dpi) noexcept
{
    SIZE spacing = {};
    const UINT resolved_source_dpi = source_dpi == 0 ? USER_DEFAULT_SCREEN_DPI : source_dpi;
    const UINT resolved_target_dpi = target_dpi == 0 ? USER_DEFAULT_SCREEN_DPI : target_dpi;
    spacing.cx = ScaleForDpi(raw_spacing.cx, resolved_source_dpi, resolved_target_dpi);
    spacing.cy = ScaleForDpi(raw_spacing.cy, resolved_source_dpi, resolved_target_dpi);
    return spacing;
}

SIZE ResolveGridSpacing(
    HWND desktop_list_view_window,
    UINT target_dpi,
    IconLayoutMetrics& metrics) noexcept
{
    metrics.used_list_view_spacing = false;
    metrics.normalized_for_target_dpi = false;
    metrics.target_dpi = target_dpi == 0 ? USER_DEFAULT_SCREEN_DPI : target_dpi;
    metrics.source_dpi = USER_DEFAULT_SCREEN_DPI;
    metrics.normalized_for_target_dpi = metrics.source_dpi != metrics.target_dpi;

    metrics.raw_grid_spacing = MakeDefaultGridSpacing();
    SIZE spacing = ScaleGridSpacing(metrics.raw_grid_spacing, metrics.source_dpi, metrics.target_dpi);
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

    const UINT window_dpi = ::GetDpiForWindow(desktop_list_view_window);
    metrics.source_dpi = window_dpi == 0 ? USER_DEFAULT_SCREEN_DPI : window_dpi;
    metrics.raw_grid_spacing.cx = horizontal_spacing;
    metrics.raw_grid_spacing.cy = vertical_spacing;
    spacing.cx = ScaleForDpi(horizontal_spacing, metrics.source_dpi, metrics.target_dpi);
    spacing.cy = ScaleForDpi(vertical_spacing, metrics.source_dpi, metrics.target_dpi);
    metrics.used_list_view_spacing = true;
    metrics.normalized_for_target_dpi = metrics.source_dpi != metrics.target_dpi;
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

struct GridGeometry final
{
    int step_x = 1;
    int step_y = 1;
    POINT phase = {};
    POINT start = {};
    POINT maximum_anchor = {};
    int available_rows = 0;
    int available_columns = 0;
    long long capacity = 0;
};

GridGeometry ResolveGridGeometry(
    const RECT& target_work_area,
    const SIZE& spacing,
    const std::vector<int>& observed_x_values,
    const std::vector<int>& observed_y_values) noexcept
{
    GridGeometry geometry;
    geometry.step_x = std::max(1, static_cast<int>(spacing.cx));
    geometry.step_y = std::max(1, static_cast<int>(spacing.cy));
    geometry.phase.x = ResolveObservedGridPhase(observed_x_values, geometry.step_x);
    geometry.phase.y = ResolveObservedGridPhase(observed_y_values, geometry.step_y);

    const int target_left = static_cast<int>(target_work_area.left);
    const int target_top = static_cast<int>(target_work_area.top);
    const int target_right = static_cast<int>(target_work_area.right);
    const int target_bottom = static_cast<int>(target_work_area.bottom);

    geometry.start.x = AlignToPhaseAtOrAfter(target_left, geometry.step_x, geometry.phase.x);
    geometry.start.y = AlignToPhaseAtOrAfter(target_top, geometry.step_y, geometry.phase.y);
    geometry.maximum_anchor.x = AlignToPhaseAtOrBefore(
        std::max(target_left, target_right - geometry.step_x),
        geometry.step_x,
        geometry.phase.x);
    geometry.maximum_anchor.y = AlignToPhaseAtOrBefore(
        std::max(target_top, target_bottom - geometry.step_y),
        geometry.step_y,
        geometry.phase.y);

    geometry.available_columns = geometry.start.x <= geometry.maximum_anchor.x
                                     ? ((geometry.maximum_anchor.x - geometry.start.x) / geometry.step_x) + 1
                                     : 0;
    geometry.available_rows = geometry.start.y <= geometry.maximum_anchor.y
                                  ? ((geometry.maximum_anchor.y - geometry.start.y) / geometry.step_y) + 1
                                  : 0;
    geometry.capacity =
        static_cast<long long>(geometry.available_columns) * static_cast<long long>(geometry.available_rows);
    return geometry;
}
}  // namespace

bool IconLayoutCalculator::Calculate(
    const RECT& target_work_area,
    int icon_count,
    IconLayoutResult& result,
    HWND desktop_list_view_window,
    UINT target_monitor_dpi,
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
        target_monitor_dpi,
        result.metrics);

    const std::vector<int> observed_x_values = ExtractUniqueAxisValues(sample_positions, true);
    const std::vector<int> observed_y_values = ExtractUniqueAxisValues(sample_positions, false);

    if (icon_count == 0)
    {
        Logger::Info(
            kIconLayoutPhase,
            L"Calculated an empty icon layout. target_work_area=" + RectToString(target_work_area));
        return true;
    }

    GridGeometry geometry = ResolveGridGeometry(
        target_work_area,
        result.metrics.grid_spacing,
        observed_x_values,
        observed_y_values);

    result.metrics.grid_phase = geometry.phase;
    result.metrics.start_position = geometry.start;
    result.metrics.available_columns = geometry.available_columns;
    result.metrics.available_rows = geometry.available_rows;
    result.metrics.icons_per_column = std::max(1, result.metrics.available_rows);
    result.metrics.column_count = ((icon_count - 1) / result.metrics.icons_per_column) + 1;

    result.positions.reserve(static_cast<std::size_t>(icon_count));

    bool coordinates_clamped = geometry.capacity < icon_count;
    for (int icon_index = 0; icon_index < icon_count; ++icon_index)
    {
        const int column_index = icon_index / result.metrics.icons_per_column;
        const int row_index = icon_index % result.metrics.icons_per_column;

        const int raw_x = geometry.start.x + (column_index * geometry.step_x);
        const int raw_y = geometry.start.y + (row_index * geometry.step_y);

        POINT position = {};
        position.x = ClampCoordinate(raw_x, geometry.start.x, geometry.maximum_anchor.x);
        position.y = ClampCoordinate(raw_y, geometry.start.y, geometry.maximum_anchor.y);

        coordinates_clamped = coordinates_clamped || position.x != raw_x || position.y != raw_y;
        result.positions.push_back(position);
    }

    result.metrics.coordinates_clamped = coordinates_clamped;

    std::wstring message = L"Calculated icon layout. target_work_area=";
    message += RectToString(target_work_area);
    message += L" icon_count=";
    message += std::to_wstring(icon_count);
    message += L" raw_grid_spacing=[";
    message += std::to_wstring(result.metrics.raw_grid_spacing.cx);
    message += L",";
    message += std::to_wstring(result.metrics.raw_grid_spacing.cy);
    message += L"] source_dpi=";
    message += std::to_wstring(result.metrics.source_dpi);
    message += L" target_dpi=";
    message += std::to_wstring(result.metrics.target_dpi);
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
    message += L" normalized_for_target_dpi=";
    message += result.metrics.normalized_for_target_dpi ? L"true" : L"false";
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
