// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <vector>

namespace defragger {

// Apply worker events to the same allocation schema returned by the mapper.
// The map and cells are updated together only after the event is validated.
void desktop_live_reset(Json& map, std::vector<Json>& cells, const Json& payload);
void desktop_live_ranges(const Json& map, std::vector<Json>& cells,
                         const Json& payload, bool plural);
void desktop_live_cells(Json& map, std::vector<Json>& cells, const Json& payload);

} // namespace defragger
