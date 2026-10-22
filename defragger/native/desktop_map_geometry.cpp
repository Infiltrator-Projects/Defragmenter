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

    // Input is the native boundary's bounded, ordered allocation-cell array.
    // Keep those cells whole: FAT clusters are never expanded into sector
    // pixels, and additional screen capacity never repeats a storage unit.
    raster.pixels = std::min(capacity, cells.size());
    raster.minimum_units = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t pixel = 0U; pixel < raster.pixels; ++pixel) {
        const std::size_t begin = pixel * cells.size() / raster.pixels;
        const std::size_t end = (pixel + 1U) * cells.size() / raster.pixels;
        const std::uint64_t span =
            number(cells[end - 1U], "end") - number(cells[begin], "start") + 1U;
        raster.minimum_units = std::min(raster.minimum_units, span);
        raster.maximum_units = std::max(raster.maximum_units, span);
    }
    return raster;
}

} // namespace defragger
