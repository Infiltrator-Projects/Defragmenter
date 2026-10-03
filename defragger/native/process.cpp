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
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace defragger {
namespace {

thread_local const volatile std::sig_atomic_t* g_cancel_flag = nullptr;

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

std::runtime_error system_error_code(const char* action, int code) {
    return std::runtime_error(
        std::string(action) + ": " + std::strerror(code));
}

void read_stream(int fd, std::string& output, std::size_t limit,
                 std::atomic<bool>& exceeded,
                 const std::function<void(const std::string&)>& observer) noexcept {
    std::array<char, 8192> buffer{};
    std::string line;
    for (;;) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count == 0) return;
        if (count < 0) {
            if (errno == EINTR) continue;
            return;
        }
        const auto amount = static_cast<std::size_t>(count);
        if (observer) {
            for (std::size_t index = 0; index < amount; ++index) {
                if (buffer[index] == '\n') {
                    try { observer(line); } catch (...) { /* Observation cannot break capture. */ }
                    line.clear();
                } else if (line.size() < 4096U) line += buffer[index];
            }
        }
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

void set_run_capture_cancel_flag(
    const volatile std::sig_atomic_t* flag) noexcept {
    g_cancel_flag = flag;
}

CommandResult run_capture(const std::vector<std::string>& command,
                          std::size_t output_limit,
                          std::chrono::milliseconds timeout,
                          const std::function<void(const std::string&)>& stderr_line) {
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

    std::vector<char*> arguments;
    arguments.reserve(command.size() + 1U);
    for (const auto& item : command)
        arguments.push_back(const_cast<char*>(item.c_str()));
    arguments.push_back(nullptr);

    std::vector<std::string> environment_storage;
    for (char** item = environ; item != nullptr && *item != nullptr; ++item) {
        std::string value(*item);
        if (value.rfind("LC_ALL=", 0U) == 0U ||
            value.rfind("LANG=", 0U) == 0U) {
            continue;
        }
        environment_storage.push_back(std::move(value));
    }
    environment_storage.emplace_back("LC_ALL=C");
    environment_storage.emplace_back("LANG=C");
    std::vector<char*> environment;
    environment.reserve(environment_storage.size() + 1U);
    for (auto& item : environment_storage)
        environment.push_back(item.data());
    environment.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    if (spawn_error != 0)
        throw system_error_code("posix_spawn_file_actions_init", spawn_error);

    auto action = [&](int code, const char* name) {
        if (code != 0) {
            (void)posix_spawn_file_actions_destroy(&actions);
            throw system_error_code(name, code);
        }
    };
    action(posix_spawn_file_actions_adddup2(
               &actions, stdout_write.get(), STDOUT_FILENO),
           "posix_spawn stdout dup");
    action(posix_spawn_file_actions_adddup2(
               &actions, stderr_write.get(), STDERR_FILENO),
           "posix_spawn stderr dup");
    action(posix_spawn_file_actions_addclose(&actions, stdout_read.get()),
           "posix_spawn stdout read close");
    action(posix_spawn_file_actions_addclose(&actions, stderr_read.get()),
           "posix_spawn stderr read close");
    action(posix_spawn_file_actions_addclose(&actions, stdout_write.get()),
           "posix_spawn stdout write close");
    action(posix_spawn_file_actions_addclose(&actions, stderr_write.get()),
           "posix_spawn stderr write close");

    posix_spawnattr_t attributes;
    spawn_error = posix_spawnattr_init(&attributes);
    if (spawn_error != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        throw system_error_code("posix_spawnattr_init", spawn_error);
    }
    const short flags = POSIX_SPAWN_SETPGROUP;
    spawn_error = posix_spawnattr_setflags(&attributes, flags);
    if (spawn_error == 0)
        spawn_error = posix_spawnattr_setpgroup(&attributes, 0);
    if (spawn_error != 0) {
        (void)posix_spawnattr_destroy(&attributes);
        (void)posix_spawn_file_actions_destroy(&actions);
        throw system_error_code("posix_spawn process group", spawn_error);
    }

    pid_t child = -1;
    spawn_error = posix_spawnp(
        &child, command.front().c_str(), &actions, &attributes,
        arguments.data(), environment.data());
    (void)posix_spawnattr_destroy(&attributes);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0)
        throw system_error_code("posix_spawnp", spawn_error);

    stdout_write.reset();
    stderr_write.reset();

    CommandResult result;
    std::atomic<bool> stdout_exceeded{false};
    std::atomic<bool> stderr_exceeded{false};
    std::thread stdout_thread(
        read_stream, stdout_read.get(), std::ref(result.standard_output),
        output_limit, std::ref(stdout_exceeded), std::function<void(const std::string&)>{});
    std::thread stderr_thread(
        read_stream, stderr_read.get(), std::ref(result.standard_error),
        output_limit, std::ref(stderr_exceeded), std::cref(stderr_line));

    const auto started = std::chrono::steady_clock::now();
    bool timed_out = false;
    bool killed_for_output = false;
    bool cancelled = false;
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
        } else if (g_cancel_flag != nullptr && *g_cancel_flag != 0) {
            cancelled = true;
            terminate_process_group(child);
        } else if (timeout > std::chrono::milliseconds::zero() &&
                   std::chrono::steady_clock::now() - started >= timeout) {
            timed_out = true;
            terminate_process_group(child);
        }

        if (killed_for_output || timed_out || cancelled) {
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
    if (cancelled)
        throw std::runtime_error("child process cancelled");
    return result;
}

} // namespace defragger
