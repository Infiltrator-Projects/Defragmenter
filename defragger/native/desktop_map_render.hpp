// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace defragger {

struct DesktopMapDisplayGeometry {
    std::uint64_t first_unit = 0U;
    std::uint64_t last_unit = 0U;
    std::uint64_t allocation_units = 0U;
    std::uint64_t display_units_per_allocation_unit = 1U;
    std::uint64_t prefix_display_units = 0U;
    std::uint64_t suffix_display_units = 0U;
    std::uint64_t total_display_units = 0U;
    std::uint64_t display_unit_size = 0U;
    std::string display_unit_name;
    bool exact_subunits = false;
};

DesktopMapDisplayGeometry desktop_map_display_geometry(
    const Json& map, const std::vector<Json>& cells);

gboolean desktop_draw_map(
    GtkWidget* widget,
    cairo_t* cr,
    const Json& map,
    const std::vector<Json>& cells,
    std::uint64_t generation);

} // namespace defragger
