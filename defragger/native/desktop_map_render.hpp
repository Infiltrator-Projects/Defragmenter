// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <vector>

namespace defragger {

gboolean desktop_draw_map(
    GtkWidget* widget,
    cairo_t* cr,
    const std::vector<Json>& cells,
    std::uint64_t generation);

} // namespace defragger
