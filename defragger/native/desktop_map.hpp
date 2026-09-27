// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <vector>

namespace defragger {

// Validate the complete mapper allocation contract before accepting a filesystem
// identity or enabling writes in the desktop client.
std::vector<Json> desktop_validate_map(const Json& map);

} // namespace defragger
