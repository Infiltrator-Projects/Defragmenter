// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_live_map.hpp"

#include <cassert>
#include <stdexcept>
#include <vector>

int main() {
    using namespace defragger;
    auto map = Json::parse(R"({"unit_size":4096,"total_units":8,"cells":[]})");
    auto cells = Json::parse(R"([
      {"start":0,"end":3,"free":2,"used":2,"fragmented":2,"directory":1},
      {"start":4,"end":7,"free":4,"used":0,"fragmented":0,"directory":0}
    ])").array();
    desktop_live_reset(map, cells, Json::parse(R"({"filesystem_units":6,"used_ranges":[[4096,8192]]})"));
    assert(cells[0].at("used").unsigned_or() == 2);
    assert(cells[0].at("free").unsigned_or() == 2);
    assert(cells[1].at("outside").unsigned_or() == 2);
    assert(map.at("filesystem_bytes").unsigned_or() == 24576);
    desktop_live_ranges(map, cells, Json::parse(R"({"source_start_byte":4096,"destination_start_byte":16384,"length_bytes":4096})"), false);
    assert(cells[0].at("used").unsigned_or() == 1);
    assert(cells[1].at("used").unsigned_or() == 1);
    auto before = cells;
    try {
        desktop_live_ranges(map, cells, Json::parse(R"({"source_start_byte":18446744073709551615,"destination_start_byte":0,"length_bytes":2})"), false);
        assert(false);
    } catch (const std::runtime_error&) { assert(Json(cells).dump() == Json(before).dump()); }
    desktop_live_reset(map, cells, Json::parse(R"({"filesystem_units":8,"block_size":4096,"free_ranges":[[0,2],[4,6]]})"));
    assert(cells[0].at("used").unsigned_or() == 2);
    assert(cells[1].at("used").unsigned_or() == 2);
    desktop_live_cells(map, cells, Json::parse(R"({"cells":[{"i":0,"start":0,"end":3,"used":3,"free":1,"fragmented":1,"directory":0,"bad":0}],"fragmented_files":12})"));
    assert(cells[0].at("used").unsigned_or() == 3);
    assert(map.at("fragmented_files").unsigned_or() == 12);
    before = cells;
    try {
        desktop_live_cells(map, cells, Json::parse(R"({"cells":[{"i":0,"start":5,"end":9}]})"));
        assert(false);
    } catch (const std::runtime_error&) { assert(Json(cells).dump() == Json(before).dump()); }
}
