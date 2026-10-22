// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace defragger {

struct DesktopMapRasterGeometry {
    std::size_t pixels = 0U;
    std::uint64_t minimum_units = 0U;
    std::uint64_t maximum_units = 0U;
};

// Painting and captions share one native-allocation geometry. Validated source
// cells are indivisible; each mapped device pixel consumes one or more cells.
DesktopMapRasterGeometry desktop_map_raster_geometry(
    const std::vector<Json>& cells,
    std::size_t capacity);

} // namespace defragger
