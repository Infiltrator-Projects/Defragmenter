// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_live_map.hpp"
#include "desktop_map_geometry.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

// Checks remain executable under NDEBUG in optimized qualification builds.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        std::exit(EXIT_FAILURE); \
    } \
} while (0)

int main() {
    using namespace defragger;
    auto map = Json::parse(R"({"unit_size":4096,"total_units":8,"cells":[]})");
    auto cells = Json::parse(R"([
      {"start":0,"end":3,"free":2,"used":2,"fragmented":2,"directory":1},
      {"start":4,"end":7,"free":4,"used":0,"fragmented":0,"directory":0}
    ])").array();
    desktop_live_reset(map, cells, Json::parse(R"({"filesystem_units":6,"used_ranges":[[4096,8192]]})"));
    CHECK(cells[0].at("used").unsigned_or() == 2);
    CHECK(cells[0].at("free").unsigned_or() == 2);
    CHECK(cells[1].at("outside").unsigned_or() == 2);
    CHECK(map.at("filesystem_bytes").unsigned_or() == 24576);
    desktop_live_ranges(map, cells, Json::parse(R"({"source_start_byte":4096,"destination_start_byte":16384,"length_bytes":4096})"), false);
    CHECK(cells[0].at("used").unsigned_or() == 1);
    CHECK(cells[1].at("used").unsigned_or() == 1);
    auto before = cells;
    try {
        desktop_live_ranges(map, cells, Json::parse(R"({"source_start_byte":18446744073709551615,"destination_start_byte":0,"length_bytes":2})"), false);
        CHECK(false);
    } catch (const std::runtime_error&) { CHECK(Json(cells).dump() == Json(before).dump()); }
    before = cells;
    try {
        desktop_live_ranges(
            map, cells,
            Json::parse(R"({"ranges":[[0,4096,4096],[18446744073709551615,0,2]]})"),
            true);
        CHECK(false);
    } catch (const std::runtime_error&) { CHECK(Json(cells).dump() == Json(before).dump()); }
    before = cells;
    desktop_live_reset(
        map, cells,
        Json::parse(R"({"reason":"authoritative post-recovery map"})"));
    CHECK(Json(cells).dump() == Json(before).dump());
    desktop_live_reset(
        map, cells,
        Json::parse(R"({"filesystem_units":8,"unit_size":4096,"used_ranges":[[0,8192],[16384,8192]]})"));
    CHECK(cells[0].at("used").unsigned_or() == 2);
    CHECK(cells[1].at("used").unsigned_or() == 2);
    desktop_live_cells(map, cells, Json::parse(R"({"cells":[{"i":0,"start":0,"end":3,"used":3,"free":1,"fragmented":1,"directory":0,"bad":0}],"fragmented_files":12})"));
    CHECK(cells[0].at("used").unsigned_or() == 3);
    CHECK(map.at("fragmented_files").unsigned_or() == 12);
    before = cells;
    try {
        desktop_live_cells(map, cells, Json::parse(R"({"cells":[{"i":0,"start":5,"end":9}]})"));
        CHECK(false);
    } catch (const std::runtime_error&) { CHECK(Json(cells).dump() == Json(before).dump()); }

    const auto grouped = Json::parse(R"([
        {"start":0,"end":5}, {"start":6,"end":11}, {"start":12,"end":13}
    ])").array();
    auto raster = desktop_map_raster_geometry(grouped, 100U);
    CHECK(raster.pixels == 100U && raster.enlarged);
    CHECK(raster.minimum_units == 2U && raster.maximum_units == 6U);
    raster = desktop_map_raster_geometry(grouped, 2U);
    CHECK(raster.pixels == 2U && !raster.enlarged);
    CHECK(raster.minimum_units == 6U && raster.maximum_units == 8U);
    CHECK(desktop_map_raster_geometry(grouped, 0U).pixels == 0U);

    const auto clusters = Json::parse(R"([
        {"start":2,"end":2}, {"start":3,"end":3}
    ])").array();
    // A small exact FAT volume must use the full available raster, restoring
    // the October 1 rendering contract without claiming finer cluster detail.
    raster = desktop_map_raster_geometry(clusters, 100U);
    CHECK(raster.pixels == 100U && raster.enlarged);
    CHECK(raster.minimum_units == 1U && raster.maximum_units == 1U);
    raster = desktop_map_raster_geometry(clusters, 1U);
    CHECK(raster.pixels == 1U && !raster.enlarged);
    CHECK(raster.minimum_units == 2U && raster.maximum_units == 2U);
    // Aggregated cells are enlarged visually too; no sub-cell allocation
    // states are inferred from their repeated colour.
    const auto aggregated = Json::parse(R"([
        {"start":0,"end":31}, {"start":32,"end":63}
    ])").array();
    raster = desktop_map_raster_geometry(aggregated, 512U);
    CHECK(raster.pixels == 512U && raster.enlarged);
    CHECK(raster.minimum_units == 32U && raster.maximum_units == 32U);
    CHECK(desktop_map_raster_geometry({}, 100U).pixels == 0U);
}
