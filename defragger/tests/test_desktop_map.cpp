// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_map.hpp"

#include <cassert>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    using namespace defragger;
    auto map = Json::parse(R"({"filesystem":"ext4","total_units":4,"cell_count":1,
        "total_bytes":16384,"used_bytes":8192,"free_bytes":8192,
        "cells":[{"start":0,"end":3,"used":2,"free":2,"bad":1,"fragmented":1,"directory":1}]})");
    assert(desktop_validate_map(map).size() == 1);
    map.at("cells").array()[0].object()["used"] = Json::integer(3);
    try { (void)desktop_validate_map(map); assert(false); }
    catch (const std::runtime_error&) {}
    map.at("cells").array()[0].object()["used"] = Json::integer(2);
    map.at("cells").array()[0].object()["end"] = Json::integer(4);
    try { (void)desktop_validate_map(map); assert(false); }
    catch (const std::runtime_error&) {}
    map.at("cells").array()[0].object()["end"] = Json::integer(2);
    try { (void)desktop_validate_map(map); assert(false); }
    catch (const std::runtime_error&) {}
    if (argc == 2) {
        std::ifstream file(argv[1]);
        assert(file);
        const std::string json(std::istreambuf_iterator<char>{file}, {});
        assert(!desktop_validate_map(Json::parse(json)).empty());
    }
}
