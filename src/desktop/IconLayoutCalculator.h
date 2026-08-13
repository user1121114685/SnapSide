#pragma once

#include <span>
#include <vector>

#include <windows.h>

namespace icon_mover
{
struct IconLayoutMetrics final
{
    SIZE raw_grid_spacing = {};
    SIZE grid_spacing = {};
    bool used_list_view_spacing = false;
    bool normalized_for_target_dpi = false;
    UINT source_dpi = USER_DEFAULT_SCREEN_DPI;
    UINT target_dpi = USER_DEFAULT_SCREEN_DPI;
    POINT grid_phase = {};
    POINT start_position = {};
    int available_rows = 0;
    int available_columns = 0;
    int icons_per_column = 0;
    int column_count = 0;
    bool coordinates_clamped = false;
};

struct IconLayoutResult final
{
    std::vector<POINT> positions;
    IconLayoutMetrics metrics;
};

class IconLayoutCalculator final
{
public:
    static bool Calculate(
        const RECT& target_work_area,
        int icon_count,
        IconLayoutResult& result,
        HWND desktop_list_view_window = nullptr,
        UINT target_monitor_dpi = USER_DEFAULT_SCREEN_DPI,
        std::span<const POINT> sample_positions = {});

private:
    IconLayoutCalculator() = delete;
};
}  // namespace icon_mover
