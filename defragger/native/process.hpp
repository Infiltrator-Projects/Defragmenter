// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <csignal>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace defragger {

struct CommandResult {
    int return_code = -1;
    std::string standard_output;
    std::string standard_error;
    bool output_truncated = false;
};

void set_run_capture_cancel_flag(
    const volatile std::sig_atomic_t* flag) noexcept;

CommandResult run_capture(
    const std::vector<std::string>& command,
    std::size_t output_limit = 64U * 1024U * 1024U,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(),
    const std::function<void(const std::string&)>& stderr_line = {});

} // namespace defragger
