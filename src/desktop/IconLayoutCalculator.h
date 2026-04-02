#pragma once

#include <span>
#include <vector>

#include <windows.h>

namespace icon_mover
{
struct IconLayoutMetrics final
{
    SIZE grid_spacing = {};
    bool used_list_view_spacing = false;
    bool used_observed_spacing = false;
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
        std::span<const POINT> sample_positions = {});

private:
    IconLayoutCalculator() = delete;
};
}  // namespace icon_mover
