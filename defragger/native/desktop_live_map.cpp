// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_live_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace defragger {
namespace {
using CellPatches = std::map<std::size_t, Json>;

std::uint64_t number(const Json& object, const char* key) {
    const auto* value = object.find(key);
    return value ? value->unsigned_or() : 0;
}
std::uint64_t unit_size(const Json& map) {
    auto value = number(map, "unit_size");
    if (!value) value = number(map, "cluster_size");
    if (!value) throw std::runtime_error("Allocation map has no unit size");
    return value;
}
std::uint64_t addition(std::uint64_t first, std::uint64_t second) {
    if (first > std::numeric_limits<std::uint64_t>::max() - second)
        throw std::runtime_error("Live allocation range overflows");
    return first + second;
}
std::uint64_t product(std::uint64_t first, std::uint64_t second) {
    if (second && first > std::numeric_limits<std::uint64_t>::max() / second)
        throw std::runtime_error("Live allocation size overflows");
    return first * second;
}
std::uint64_t ceiling_units(std::uint64_t bytes, std::uint64_t unit) {
    return bytes / unit + (bytes % unit != 0);
}
std::uint64_t pair_value(const Json& pair, size_t index) {
    if (!pair.is_array() || pair.array().size() <= index)
        throw std::runtime_error("Invalid live allocation pair");
    return pair.array()[index].unsigned_or();
}
void validate_cell_count(const std::vector<Json>& cells) {
    if (cells.empty() || cells.size() > 1048576)
        throw std::runtime_error("Invalid live allocation cell count");
}
void validate_cells(const std::vector<Json>& cells) {
    validate_cell_count(cells);
    std::uint64_t previous = 0;
    bool first = true;
    for (const auto& cell : cells) {
        if (!cell.is_object() || number(cell, "end") < number(cell, "start") ||
            (!first && number(cell, "start") <= previous))
            throw std::runtime_error("Live allocation cells are not ordered");
        previous = number(cell, "end");
        first = false;
    }
}
const Json& effective_cell(const std::vector<Json>& cells,
                           const CellPatches& patches,
                           std::size_t index) {
    const auto found = patches.find(index);
    return found == patches.end() ? cells[index] : found->second;
}
Json& editable_cell(const std::vector<Json>& cells,
                    CellPatches& patches,
                    std::size_t index) {
    auto found = patches.find(index);
    if (found == patches.end())
        found = patches.emplace(index, cells[index]).first;
    return found->second;
}
void apply_range(const std::vector<Json>& cells, CellPatches& patches,
                 std::uint64_t first_byte, std::uint64_t length,
                 std::uint64_t unit, bool used) {
    if (!length) return;
    const auto first = first_byte / unit;
    const auto last = ceiling_units(addition(first_byte, length), unit);
    auto cell = std::lower_bound(cells.begin(), cells.end(), first,
        [](const Json& entry, std::uint64_t start) { return number(entry, "end") < start; });
    while (cell != cells.end() && number(*cell, "start") < last) {
        const std::size_t index = static_cast<std::size_t>(cell - cells.begin());
        Json& changed = editable_cell(cells, patches, index);
        const auto begin = number(changed, "start");
        const auto end = addition(number(changed, "end"), 1);
        const auto overlap = std::min(end, last) - std::max(begin, first);
        if (overlap) {
            auto& values = changed.object();
            const auto available = number(changed, used ? "free" : "used");
            const auto moved = std::min(overlap, available);
            if (!used) {
                const auto old_used = std::max<std::uint64_t>(1, available);
                auto proportional = [&](const char* key) {
                    const auto value = number(changed, key);
                    const auto scaled = std::round(static_cast<long double>(moved) *
                                                   value / old_used);
                    return static_cast<std::uint64_t>(
                        std::min(static_cast<long double>(value), scaled));
                };
                const auto fragments = proportional("fragmented");
                const auto directories = proportional("directory");
                values["fragmented"] = Json::unsigned_integer(
                    std::min(number(changed, "fragmented") -
                                 std::min(number(changed, "fragmented"), fragments),
                             available - moved));
                values["directory"] = Json::unsigned_integer(
                    std::min(number(changed, "directory") -
                                 std::min(number(changed, "directory"), directories),
                             available - moved));
            }
            values[used ? "free" : "used"] = Json::unsigned_integer(available - moved);
            const auto other = used ? "used" : "free";
            values[other] = Json::unsigned_integer(addition(number(changed, other), moved));
        }
        ++cell;
    }
}
void commit_patches(std::vector<Json>& cells, CellPatches& patches) {
    for (auto& [index, cell] : patches)
        cells[index] = std::move(cell);
}
void validate_patched_cell_order(const std::vector<Json>& cells,
                                 const CellPatches& patches) {
    for (const auto& [index, cell] : patches) {
        if (!cell.is_object() || number(cell, "end") < number(cell, "start"))
            throw std::runtime_error("Live allocation cells are not ordered");
        if (index > 0U &&
            number(effective_cell(cells, patches, index - 1U), "end") >=
                number(cell, "start"))
            throw std::runtime_error("Live allocation cells are not ordered");
        if (index + 1U < cells.size() &&
            number(effective_cell(cells, patches, index + 1U), "start") <=
                number(cell, "end"))
            throw std::runtime_error("Live allocation cells are not ordered");
    }
}
} // namespace

void desktop_live_reset(Json& map, std::vector<Json>& cells, const Json& payload) {
    validate_cells(cells);
    auto unit = number(payload, "unit_size");
    if (!unit) unit = number(payload, "block_size");
    if (!unit) unit = unit_size(map);
    auto total = number(payload, "filesystem_units");
    if (!total) total = number(payload, "total_blocks");
    if (!total || total > addition(number(cells.back(), "end"), 1))
        throw std::runtime_error("Invalid live reset filesystem extent");
    auto changed = cells;
    for (auto& cell : changed) {
        const auto begin = number(cell, "start");
        const auto end = addition(number(cell, "end"), 1);
        const auto inside = begin >= total ? 0 : std::min(end, total) - begin;
        auto& values = cell.object();
        values["free"] = Json::unsigned_integer(inside);
        values["used"] = Json::unsigned_integer(0);
        values["unknown"] = Json::unsigned_integer(0);
        values["outside"] = Json::unsigned_integer(end - begin - inside);
        values["fragmented"] = Json::unsigned_integer(0);
        values["directory"] = Json::unsigned_integer(0);
        values["bad"] = Json::unsigned_integer(0);
    }
    auto mark = [&](std::uint64_t begin, std::uint64_t length) {
        const auto limit = product(total, unit);
        if (begin >= limit) return;
        const auto first = begin / unit;
        const auto last = ceiling_units(addition(begin, std::min(length, limit - begin)), unit);
        auto cell = std::lower_bound(changed.begin(), changed.end(), first,
            [](const Json& entry, std::uint64_t start) { return number(entry, "end") < start; });
        while (cell != changed.end() && number(*cell, "start") < last) {
            const auto cell_begin = number(*cell, "start");
            const auto cell_end = addition(number(*cell, "end"), 1);
            const auto overlap = std::min(cell_end, last) - std::max(cell_begin, first);
            if (overlap) {
                auto& values = cell->object();
                const auto available = number(*cell, "free");
                const auto moved = std::min(overlap, available);
                values["free"] = Json::unsigned_integer(available - moved);
                values["used"] = Json::unsigned_integer(addition(number(*cell, "used"), moved));
            }
            ++cell;
        }
    };
    if (const auto* used = payload.find("used_ranges"); used && used->is_array()) {
        if (used->array().size() > 1048576) throw std::runtime_error("Too many used ranges");
        for (const auto& range : used->array()) {
            if (!range.is_array() || range.array().size() != 2) continue;
            mark(pair_value(range, 0), pair_value(range, 1));
        }
    } else {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> free_ranges;
        if (const auto* free = payload.find("free_ranges"); free && free->is_array()) {
            if (free->array().size() > 1048576) throw std::runtime_error("Too many free ranges");
            for (const auto& range : free->array()) {
                if (!range.is_array() || range.array().size() != 2) continue;
                auto begin = std::min(total, pair_value(range, 0));
                auto end = std::min(total, pair_value(range, 1));
                free_ranges.emplace_back(begin, std::max(begin, end));
            }
        }
        std::sort(free_ranges.begin(), free_ranges.end());
        std::uint64_t cursor = 0;
        for (const auto& [begin, end] : free_ranges) {
            if (begin > cursor) mark(product(cursor, unit), product(begin - cursor, unit));
            cursor = std::max(cursor, end);
        }
        if (cursor < total) mark(product(cursor, unit), product(total - cursor, unit));
    }
    auto updated = map;
    updated.object()["filesystem_units"] = Json::unsigned_integer(total);
    updated.object()["filesystem_bytes"] = Json::unsigned_integer(product(total, unit));
    updated.object()["outside_bytes"] = Json::unsigned_integer(
        product(number(map, "total_units") > total ? number(map, "total_units") - total : 0, unit));
    cells.swap(changed);
    map = std::move(updated);
}

void desktop_live_ranges(const Json& map, std::vector<Json>& cells,
                         const Json& payload, bool plural) {
    validate_cell_count(cells);
    const auto unit = unit_size(map);
    const bool allocate_only = payload.find("mode") &&
        payload.at("mode").string_or() == "allocate-only";
    CellPatches patches;
    auto move = [&](std::uint64_t source, std::uint64_t destination, std::uint64_t length) {
        if (!allocate_only) apply_range(cells, patches, source, length, unit, false);
        apply_range(cells, patches, destination, length, unit, true);
    };
    if (!plural) {
        move(number(payload, "source_start_byte"),
             number(payload, "destination_start_byte"),
             number(payload, "length_bytes"));
    } else if (const auto* ranges = payload.find("ranges"); ranges && ranges->is_array()) {
        if (ranges->array().size() > 1048576) throw std::runtime_error("Too many live ranges");
        for (const auto& range : ranges->array())
            if (range.is_array() && range.array().size() == 3)
                move(pair_value(range, 0), pair_value(range, 1), pair_value(range, 2));
    }
    commit_patches(cells, patches);
}

void desktop_live_cells(Json& map, std::vector<Json>& cells, const Json& payload) {
    validate_cell_count(cells);
    if (!map.is_object()) throw std::runtime_error("Invalid live allocation map");
    CellPatches patches;
    if (const auto* updates = payload.find("cells"); updates && updates->is_array()) {
        if (updates->array().size() > cells.size()) throw std::runtime_error("Too many live cells");
        for (const auto& update : updates->array()) {
            if (!update.is_object()) continue;
            const auto index = number(update, "i");
            if (index >= cells.size()) continue;
            Json::Object cell;
            for (const auto* key : {"start", "end", "free", "used", "unknown", "outside",
                                    "fragmented", "directory", "bad"})
                cell[key] = Json::unsigned_integer(number(update, key));
            patches[static_cast<std::size_t>(index)] = Json(std::move(cell));
        }
    }
    validate_patched_cell_order(cells, patches);
    commit_patches(cells, patches);
    for (const auto* key : {"fragmented_files", "fragmented_directories", "free_clusters",
                            "free_gaps_below_highest"})
        if (const auto* value = payload.find(key)) map.object()[key] = *value;
}
} // namespace defragger