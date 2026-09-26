// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace defragger {

struct CommandResult {
    int return_code = -1;
    std::string standard_output;
    std::string standard_error;
    bool output_truncated = false;
};

CommandResult run_capture(
    const std::vector<std::string>& command,
    std::size_t output_limit = 64U * 1024U * 1024U,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

} // namespace defragger
