// SPDX-License-Identifier: GPL-3.0-or-later
#include "process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace defragger {
namespace {

class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    int get() const noexcept { return value_; }
    int release() noexcept {
        const int value = value_;
        value_ = -1;
        return value;
    }
    void reset(int value = -1) noexcept {
        if (value_ >= 0) (void)close(value_);
        value_ = value;
    }
private:
    int value_;
};

std::runtime_error system_error(const char* action) {
    return std::runtime_error(
        std::string(action) + ": " + std::strerror(errno));
}

void read_stream(int fd, std::string& output, std::size_t limit,
                 std::atomic<bool>& exceeded) noexcept {
    std::array<char, 8192> buffer{};
    for (;;) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count == 0) return;
        if (count < 0) {
            if (errno == EINTR) continue;
            return;
        }
        const auto amount = static_cast<std::size_t>(count);
        const std::size_t available =
            output.size() < limit ? limit - output.size() : 0U;
        const std::size_t kept = std::min(available, amount);
        if (kept != 0U) output.append(buffer.data(), kept);
        if (kept < amount) {
            exceeded.store(true, std::memory_order_release);
            return;
        }
    }
}

void terminate_process_group(pid_t child) noexcept {
    if (child <= 0) return;
    if (kill(-child, SIGKILL) != 0 && errno == ESRCH)
        (void)kill(child, SIGKILL);
}

} // namespace

CommandResult run_capture(const std::vector<std::string>& command,
                          std::size_t output_limit,
                          std::chrono::milliseconds timeout) {
    if (command.empty() || command.front().empty())
        throw std::invalid_argument("cannot run an empty command");

    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    if (pipe(stdout_pipe) != 0) throw system_error("pipe stdout");
    Fd stdout_read(stdout_pipe[0]);
    Fd stdout_write(stdout_pipe[1]);
    if (pipe(stderr_pipe) != 0) throw system_error("pipe stderr");
    Fd stderr_read(stderr_pipe[0]);
    Fd stderr_write(stderr_pipe[1]);

    const pid_t child = fork();
    if (child < 0) throw system_error("fork");
    if (child == 0) {
        if (setpgid(0, 0) != 0)
            _exit(126);
        if (dup2(stdout_write.get(), STDOUT_FILENO) < 0 ||
            dup2(stderr_write.get(), STDERR_FILENO) < 0) {
            _exit(126);
        }
        stdout_read.reset();
        stdout_write.reset();
        stderr_read.reset();
        stderr_write.reset();
        (void)setenv("LC_ALL", "C", 1);
        (void)setenv("LANG", "C", 1);

        std::vector<char*> arguments;
        arguments.reserve(command.size() + 1U);
        for (const auto& item : command)
            arguments.push_back(const_cast<char*>(item.c_str()));
        arguments.push_back(nullptr);
        execv(arguments.front(), arguments.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    stdout_write.reset();
    stderr_write.reset();

    CommandResult result;
    std::atomic<bool> stdout_exceeded{false};
    std::atomic<bool> stderr_exceeded{false};
    std::thread stdout_thread(
        read_stream, stdout_read.get(), std::ref(result.standard_output),
        output_limit, std::ref(stdout_exceeded));
    std::thread stderr_thread(
        read_stream, stderr_read.get(), std::ref(result.standard_error),
        output_limit, std::ref(stderr_exceeded));

    const auto started = std::chrono::steady_clock::now();
    bool timed_out = false;
    bool killed_for_output = false;
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) {
            terminate_process_group(child);
            stdout_thread.join();
            stderr_thread.join();
            throw system_error("waitpid");
        }

        if (stdout_exceeded.load(std::memory_order_acquire) ||
            stderr_exceeded.load(std::memory_order_acquire)) {
            killed_for_output = true;
            terminate_process_group(child);
        } else if (timeout > std::chrono::milliseconds::zero() &&
                   std::chrono::steady_clock::now() - started >= timeout) {
            timed_out = true;
            terminate_process_group(child);
        }

        if (killed_for_output || timed_out) {
            for (;;) {
                const pid_t reaped = waitpid(child, &status, 0);
                if (reaped == child) break;
                if (reaped < 0 && errno == EINTR) continue;
                break;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stdout_thread.join();
    stderr_thread.join();
    result.output_truncated = killed_for_output ||
        stdout_exceeded.load(std::memory_order_acquire) ||
        stderr_exceeded.load(std::memory_order_acquire);

    if (WIFEXITED(status))
        result.return_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.return_code = 128 + WTERMSIG(status);
    else
        result.return_code = 126;

    if (result.output_truncated)
        throw std::runtime_error("child process output exceeded safety limit");
    if (timed_out)
        throw std::runtime_error("child process exceeded execution-time limit");
    return result;
}

} // namespace defragger
