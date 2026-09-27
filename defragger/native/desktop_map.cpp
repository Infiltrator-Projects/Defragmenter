// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_map.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace defragger {
namespace {
std::uint64_t integer(const Json& object, const char* key, bool required = true) {
    const auto* value = object.find(key);
    if (!value) {
        if (!required) return 0;
        throw std::runtime_error(std::string("Allocation map is missing ") + key);
    }
    if (!value->is_number()) throw std::runtime_error(std::string("Invalid allocation map field: ") + key);
    return value->unsigned_value();
}
}

std::vector<Json> desktop_validate_map(const Json& map) {
    if (!map.is_object()) throw std::runtime_error("Allocation map root must be an object");
    const auto* filesystem = map.find("filesystem");
    if (!filesystem || !filesystem->is_string() ||
        !backend_by_fstype(filesystem->string()))
        throw std::runtime_error("Native analyser returned an unknown filesystem identity");
    const auto units = integer(map, "total_units");
    if (!units) throw std::runtime_error("Allocation map has no units");
    const auto count = integer(map, "cell_count");
    if (!count || count > 1048576) throw std::runtime_error("Allocation map cell count is out of bounds");
    const auto* cells = map.find("cells");
    if (!cells || !cells->is_array() || cells->array().size() != count)
        throw std::runtime_error("Allocation map cell count differs from the returned cells");
    const auto total = integer(map, "total_bytes");
    const auto free = integer(map, "free_bytes");
    const auto used = integer(map, "used_bytes");
    if (free > total || used > total || free > total - used)
        throw std::runtime_error("Allocation map capacity metrics are inconsistent");

    std::uint64_t previous_end = 0;
    bool first = true;
    for (const auto& cell : cells->array()) {
        if (!cell.is_object()) throw std::runtime_error("Allocation map cell is not an object");
        const auto start = integer(cell, "start");
        const auto end = integer(cell, "end");
        if (start > end || end >= units || (first ? start != 0 :
            previous_end == std::numeric_limits<std::uint64_t>::max() || start != previous_end + 1))
            throw std::runtime_error("Allocation map cell bounds are invalid or out of order");
        const auto span = end - start + 1;
        std::uint64_t primary = 0;
        for (const auto* key : {"free", "used", "unknown", "outside"}) {
            const auto value = integer(cell, key, false);
            if (value > span || primary > span - value)
                throw std::runtime_error("Allocation map cell states exceed their physical range");
            primary += value;
        }
        const auto bad = integer(cell, "bad", false);
        const auto used_units = integer(cell, "used", false);
        const bool overlay_bad = primary == span && bad <= used_units;
        const bool primary_bad = bad <= span && primary == span - bad;
        if ((!overlay_bad && !primary_bad) ||
            integer(cell, "fragmented", false) > used_units ||
            integer(cell, "directory", false) > used_units)
            throw std::runtime_error("Allocation map cell states do not cover their physical range");
        previous_end = end;
        first = false;
    }
    if (previous_end != units - 1)
        throw std::runtime_error("Allocation map does not cover the full physical range");
    return cells->array();
}
} // namespace defragger
