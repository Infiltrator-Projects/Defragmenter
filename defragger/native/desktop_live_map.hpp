// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <vector>

namespace defragger {

// Apply worker events to the validated allocation schema returned by the mapper.
// Whole-map resets remain transactional; range/cell events stage only the cells
// they touch and publish those sparse patches after the complete event validates.
void desktop_live_reset(Json& map, std::vector<Json>& cells, const Json& payload);
void desktop_live_ranges(const Json& map, std::vector<Json>& cells,
                         const Json& payload, bool plural);
void desktop_live_cells(Json& map, std::vector<Json>& cells, const Json& payload);

} // namespace defragger