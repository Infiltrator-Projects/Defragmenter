// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_map_geometry.hpp"

#include <algorithm>
#include <limits>

namespace defragger {
namespace {
std::uint64_t number(const Json& data, const char* key) {
    const auto* value = data.find(key);
    return value ? value->unsigned_or() : 0U;
}
} // namespace

DesktopMapRasterGeometry desktop_map_raster_geometry(
    const std::vector<Json>& cells, std::size_t capacity)
{
    DesktopMapRasterGeometry raster;
    if (capacity == 0U || cells.empty()) return raster;

    // Restore the full-surface contract from 2026-10-01: all native device
    // pixels represent an ordered part of the analysed physical address span.
    // Fewer cells than pixels means visual magnification of the *same* source
    // allocation state. It is not subdivision or newly discovered disk detail.
    raster.pixels = capacity;
    raster.enlarged = cells.size() < capacity;
    raster.minimum_units = std::numeric_limits<std::uint64_t>::max();
    if (raster.enlarged) {
        for (const auto& cell : cells) {
            const std::uint64_t start = number(cell, "start");
            const std::uint64_t end = number(cell, "end");
            const std::uint64_t span = end - start + 1U;
            raster.minimum_units = std::min(raster.minimum_units, span);
            raster.maximum_units = std::max(raster.maximum_units, span);
        }
    } else {
        for (std::size_t pixel = 0U; pixel < raster.pixels; ++pixel) {
            const std::size_t begin = pixel * cells.size() / raster.pixels;
            const std::size_t end = (pixel + 1U) * cells.size() / raster.pixels;
            const std::uint64_t span =
                number(cells[end - 1U], "end") - number(cells[begin], "start") + 1U;
            raster.minimum_units = std::min(raster.minimum_units, span);
            raster.maximum_units = std::max(raster.maximum_units, span);
        }
    }
    return raster;
}

} // namespace defragger
