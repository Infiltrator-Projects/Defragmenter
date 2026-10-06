// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace defragger {

class PrivilegedHelperTransport {
public:
    using LineCallback = std::function<void(std::string)>;
    using ClosedCallback = std::function<void(std::string)>;

    PrivilegedHelperTransport(std::string helper_path,
                              LineCallback on_line,
                              ClosedCallback on_closed);
    ~PrivilegedHelperTransport();

    PrivilegedHelperTransport(const PrivilegedHelperTransport&) = delete;
    PrivilegedHelperTransport& operator=(const PrivilegedHelperTransport&) = delete;

    bool active() const noexcept;
    void start();
    void send(const Json& request);
    void shutdown() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
    static void read_next(const std::shared_ptr<State>& state);
};

class LocalAnalysisTransport {
public:
    using LineCallback = std::function<void(std::string, bool)>;
    using FinishedCallback = std::function<void(int, std::string)>;

    LocalAnalysisTransport(LineCallback on_line,
                           FinishedCallback on_finished);
    ~LocalAnalysisTransport();

    LocalAnalysisTransport(const LocalAnalysisTransport&) = delete;
    LocalAnalysisTransport& operator=(const LocalAnalysisTransport&) = delete;

    bool active() const noexcept;
    void start(const std::vector<std::string>& arguments);
    void signal(int signal_number) noexcept;
    void shutdown() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
    static void read_stream(const std::shared_ptr<State>& state,
                            void* stream,
                            bool diagnostics);
    static void finish_if_ready(const std::shared_ptr<State>& state);
};

} // namespace defragger
