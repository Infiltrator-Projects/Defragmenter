// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace defragger {

struct DesktopMapRasterGeometry {
    std::size_t pixels = 0U;
    // Repeated pixels are display magnification, not extra allocation detail.
    bool enlarged = false;
    std::uint64_t minimum_units = 0U;
    std::uint64_t maximum_units = 0U;
};

// Painting and captions share the original full-surface raster contract.
// Source cells remain indivisible; they may be magnified across adjacent
// display pixels without claiming finer physical allocation resolution.
DesktopMapRasterGeometry desktop_map_raster_geometry(
    const std::vector<Json>& cells,
    std::size_t capacity);

} // namespace defragger
