// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace defragger {

inline constexpr std::size_t kMaxMapCells = 1048576U;

// Keep the transport bounded while allowing every GUI-supported map size.
std::size_t map_capture_limit(std::size_t cells);

struct UnitRange {
    std::uint64_t start = 0U;
    std::uint64_t end = 0U;
};

struct StateRange {
    std::uint64_t start = 0U;
    std::uint64_t end = 0U;
    unsigned state = 2U;
};

std::vector<UnitRange> merge_ranges(std::vector<UnitRange> ranges);
std::vector<UnitRange> complement_ranges(
    std::uint64_t total_units,
    const std::vector<UnitRange>& occupied);

Json aggregate_ranges(
    std::uint64_t total_units,
    std::size_t cell_count,
    std::uint64_t unit_size,
    std::string filesystem,
    std::vector<StateRange> ranges,
    std::string accuracy,
    Json::Object details = {});

std::uint64_t overlay_ranges(
    Json::Array& cells,
    std::vector<UnitRange> ranges,
    std::string_view field);

std::string backend_identified_filesystem(
    const BackendInfo& backend,
    const std::string& path);
bool backend_probe(const BackendInfo& backend, const std::string& path);
Json map_backend(
    const BackendInfo& backend,
    const std::string& path,
    std::size_t cells);

} // namespace defragger
