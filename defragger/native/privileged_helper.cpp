// SPDX-License-Identifier: GPL-3.0-or-later
#include "helper_policy.hpp"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <regex>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace defragger {
namespace {

constexpr std::uint64_t kProtocolVersion = 1U;
constexpr std::size_t kMaxRequestBytes = 64U * 1024U;
constexpr std::size_t kMaxArgumentCount = 128U;
constexpr std::size_t kMaxArgumentBytes = 4096U;

enum class RequestLineStatus {
    End,
    Ok,
    TooLarge,
};

RequestLineStatus read_request_line(FILE* stream, std::string& line) {
    line.clear();
    for (;;) {
        const int value = std::fgetc(stream);
        if (value == EOF)
            return line.empty() ? RequestLineStatus::End : RequestLineStatus::Ok;
        if (value == '\n')
            return RequestLineStatus::Ok;
        if (line.size() >= kMaxRequestBytes) {
            while (value != '\n') {
                const int discard = std::fgetc(stream);
                if (discard == EOF || discard == '\n') break;
            }
            line.clear();
            return RequestLineStatus::TooLarge;
        }
        line.push_back(static_cast<char>(value));
    }
}

class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { if (value_ >= 0) (void)close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return value_; }
    int release() noexcept {
        const int value = value_;
        value_ = -1;
        return value;
    }
private:
    int value_;
};

class SpawnActions {
public:
    SpawnActions() {
        const int result = posix_spawn_file_actions_init(&value_);
        if (result != 0)
            throw std::runtime_error(
                std::string("posix_spawn file-actions init failed: ") +
                std::strerror(result));
        initialised_ = true;
    }
    ~SpawnActions() {
        if (initialised_) (void)posix_spawn_file_actions_destroy(&value_);
    }
    SpawnActions(const SpawnActions&) = delete;
    SpawnActions& operator=(const SpawnActions&) = delete;
    posix_spawn_file_actions_t* get() noexcept { return &value_; }
private:
    posix_spawn_file_actions_t value_{};
    bool initialised_ = false;
};

class SpawnAttributes {
public:
    SpawnAttributes() {
        const int result = posix_spawnattr_init(&value_);
        if (result != 0)
            throw std::runtime_error(
                std::string("posix_spawn attributes init failed: ") +
                std::strerror(result));
        initialised_ = true;
    }
    ~SpawnAttributes() {
        if (initialised_) (void)posix_spawnattr_destroy(&value_);
    }
    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;
    posix_spawnattr_t* get() noexcept { return &value_; }
private:
    posix_spawnattr_t value_{};
    bool initialised_ = false;
};

void require_spawn_success(int result, const char* action) {
    if (result != 0)
        throw std::runtime_error(
            std::string(action) + ": " + std::strerror(result));
}

Json id_value(const Json& message) {
    const Json* id = message.find("id");
    return id == nullptr ? Json(nullptr) : *id;
}

std::int64_t request_id(const Json& message) {
    const Json* id = message.find("id");
    return id == nullptr ? 0 : id->integer_value();
}

std::vector<std::string> string_array(const Json& value) {
    if (!value.is_array())
        throw std::runtime_error("argv must be a list of strings");
    if (value.array().size() > kMaxArgumentCount)
        throw std::runtime_error("argv contains too many arguments");
    std::vector<std::string> result;
    result.reserve(value.array().size());
    for (const auto& item : value.array()) {
        if (!item.is_string())
            throw std::runtime_error("argv must be a list of strings");
        if (item.string().size() > kMaxArgumentBytes)
            throw std::runtime_error("argv argument exceeds protocol limit");
        result.emplace_back(item.string());
    }
    return result;
}

std::vector<std::string> child_environment() {
    std::vector<std::string> result;
    if (environ != nullptr) {
        for (char** item = environ; *item != nullptr; ++item) {
            const std::string_view value(*item);
            if (value.rfind("LC_ALL=", 0U) == 0U ||
                value.rfind("LANG=", 0U) == 0U) {
                continue;
            }
            result.emplace_back(*item);
        }
    }
    result.emplace_back("LC_ALL=C");
    result.emplace_back("LANG=C");
    return result;
}

pid_t spawn_command(const HelperCommand& allowed, int read_fd, int write_fd) {
    std::vector<std::string> command;
    command.reserve(allowed.arguments.size() + 1U);
    command.push_back(allowed.executable);
    command.insert(command.end(), allowed.arguments.begin(),
                   allowed.arguments.end());

    std::vector<char*> raw;
    raw.reserve(command.size() + 1U);
    for (auto& item : command) raw.push_back(item.data());
    raw.push_back(nullptr);

    std::vector<std::string> environment = child_environment();
    std::vector<char*> raw_environment;
    raw_environment.reserve(environment.size() + 1U);
    for (auto& item : environment) raw_environment.push_back(item.data());
    raw_environment.push_back(nullptr);

    SpawnActions actions;
    require_spawn_success(
        posix_spawn_file_actions_addopen(
            actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0),
        "posix_spawn stdin isolation failed");
    require_spawn_success(
        posix_spawn_file_actions_adddup2(
            actions.get(), write_fd, STDOUT_FILENO),
        "posix_spawn stdout redirection failed");
    require_spawn_success(
        posix_spawn_file_actions_adddup2(
            actions.get(), write_fd, STDERR_FILENO),
        "posix_spawn stderr redirection failed");
    require_spawn_success(
        posix_spawn_file_actions_addclose(actions.get(), read_fd),
        "posix_spawn read-pipe close failed");
    require_spawn_success(
        posix_spawn_file_actions_addclose(actions.get(), write_fd),
        "posix_spawn write-pipe close failed");

    SpawnAttributes attributes;
    sigset_t defaults;
    if (sigemptyset(&defaults) != 0 || sigaddset(&defaults, SIGPIPE) != 0)
        throw std::runtime_error(
            std::string("preparing child signal defaults failed: ") +
            std::strerror(errno));
    require_spawn_success(
        posix_spawnattr_setsigdefault(attributes.get(), &defaults),
        "posix_spawn signal-default setup failed");
    require_spawn_success(
        posix_spawnattr_setpgroup(attributes.get(), 0),
        "posix_spawn process-group setup failed");
    short flags = static_cast<short>(
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
    require_spawn_success(
        posix_spawnattr_setflags(attributes.get(), flags),
        "posix_spawn flags setup failed");

    pid_t child = -1;
    const int result = posix_spawn(
        &child, allowed.executable.c_str(), actions.get(), attributes.get(),
        raw.data(), raw_environment.data());
    require_spawn_success(result, "posix_spawn failed");
    return child;
}

int wait_for_child(pid_t child) {
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, 0);
        if (waited == child) return status;
        if (waited < 0 && errno == EINTR) continue;
        throw std::runtime_error(
            std::string("waitpid failed: ") + std::strerror(errno));
    }
}

void stop_and_reap(pid_t child) noexcept {
    if (child <= 0) return;
    if (kill(-child, SIGINT) != 0 && errno != ESRCH) {
        // Waiting is still mandatory. The child may have already exited or
        // may honour a signal delivered by another shutdown path.
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

class Helper {
public:
    explicit Helper(std::uint32_t invoking_uid)
        : invoking_uid_(invoking_uid) {}

    ~Helper() {
        try { stop_active_and_wait(); } catch (...) {}
    }

    int run() {
        if (!emit(Json::Object{
                {"type", Json("ready")},
                {"protocol", Json::unsigned_integer(kProtocolVersion)},
                {"pid", Json::unsigned_integer(
                    static_cast<std::uint64_t>(getpid()))}})) {
            return 1;
        }

        std::string line;
        while (!transport_failed_.load(std::memory_order_acquire)) {
            const RequestLineStatus status = read_request_line(stdin, line);
            if (status == RequestLineStatus::End)
                break;
            if (status == RequestLineStatus::TooLarge) {
                (void)fail(Json(nullptr),
                           "invalid request: protocol frame exceeds size limit");
                continue;
            }
            try {
                Json message = Json::parse(line);
                if (!message.is_object())
                    throw std::runtime_error("request must be an object");
                const Json* action_value = message.find("action");
                if (action_value == nullptr)
                    throw std::runtime_error("request is missing action");
                const std::string action(action_value->string());
                if (action == "run") {
                    handle_run(message);
                } else if (action == "stop") {
                    handle_stop(message);
                } else if (action == "ping") {
                    (void)emit(Json::Object{
                        {"type", Json("pong")},
                        {"id", id_value(message)}});
                } else if (action == "quit") {
                    stop_active_and_wait();
                    (void)emit(Json::Object{{"type", Json("bye")}});
                    return 0;
                } else {
                    (void)fail(id_value(message), "unknown helper action");
                }
            } catch (const std::exception& error) {
                (void)fail(Json(nullptr),
                           std::string("invalid request: ") + error.what());
            }
        }
        stop_active_and_wait();
        return transport_failed_.load(std::memory_order_acquire) ? 1 : 0;
    }

private:
    std::uint32_t invoking_uid_;
    std::mutex emit_mutex_;
    std::mutex active_mutex_;
    std::thread worker_;
    std::atomic<bool> transport_failed_{false};
    pid_t active_pid_ = -1;
    std::int64_t active_id_ = 0;
    bool active_has_output_ = false;
    bool active_waits_for_output_ = false;
    bool pending_stop_ = false;
    bool worker_running_ = false;

    bool emit(Json::Object object) noexcept {
        if (transport_failed_.load(std::memory_order_acquire)) return false;
        try {
            const std::string encoded = Json(std::move(object)).dump();
            std::lock_guard<std::mutex> lock(emit_mutex_);
            if (transport_failed_.load(std::memory_order_relaxed)) return false;
            const std::size_t written =
                std::fwrite(encoded.data(), 1U, encoded.size(), stdout);
            const bool ok =
                written == encoded.size() &&
                std::fputc('\n', stdout) != EOF &&
                std::fflush(stdout) == 0;
            if (!ok)
                transport_failed_.store(true, std::memory_order_release);
            return ok;
        } catch (...) {
            transport_failed_.store(true, std::memory_order_release);
            return false;
        }
    }

    bool fail(Json id, const std::string& message) noexcept {
        return emit(Json::Object{
            {"type", Json("error")},
            {"id", std::move(id)},
            {"message", Json(message)}});
    }

    void handle_run(const Json& message) {
        const std::int64_t id = request_id(message);
        const std::string program =
            message.find("program") != nullptr
                ? message.at("program").string_or() : "";
        const Json* raw_arguments = message.find("argv");
        if (raw_arguments == nullptr) {
            (void)fail(Json::integer(id), "argv must be a list of strings");
            (void)emit(Json::Object{{"type", Json("finished")},
                {"id", Json::integer(id)}, {"returncode", Json::integer(127)}});
            return;
        }

        std::vector<std::string> arguments;
        try {
            arguments = string_array(*raw_arguments);
        } catch (const std::exception& error) {
            (void)fail(Json::integer(id), error.what());
            (void)emit(Json::Object{{"type", Json("finished")},
                {"id", Json::integer(id)}, {"returncode", Json::integer(127)}});
            return;
        }

        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (worker_running_ || active_pid_ > 0) {
                (void)fail(Json::integer(id),
                           "another privileged operation is already active");
                (void)emit(Json::Object{{"type", Json("finished")},
                    {"id", Json::integer(id)}, {"returncode", Json::integer(127)}});
                return;
            }
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            worker_running_ = true;
            active_id_ = id;
            active_waits_for_output_ = program == "operation-engine";
            pending_stop_ = false;
        }

        try {
            worker_ = std::thread(
                [this, id, program, arguments = std::move(arguments)] {
                    run_request(id, program, arguments);
                });
        } catch (...) {
            std::lock_guard<std::mutex> lock(active_mutex_);
            worker_running_ = false;
            throw;
        }
    }

    void run_request(std::int64_t id, const std::string& program,
                     const std::vector<std::string>& arguments) noexcept {
        pid_t child = -1;
        bool reaped = false;
        int return_code = 127;
        try {
            HelperCommand allowed =
                helper_command(program, arguments, invoking_uid_);
#ifdef LD_PRIVILEGED_HELPER_TEST_MODE
            // Test binaries alone can substitute a controlled child after the
            // production request policy has validated the complete command.
            if (const char* test_child = std::getenv("LD_HELPER_TEST_CHILD");
                test_child != nullptr && *test_child != '\0')
                allowed.executable = test_child;
#endif
            if (access(allowed.executable.c_str(), X_OK) != 0) {
                throw std::runtime_error(
                    "helper command is unavailable: " + allowed.executable);
            }

            int output_pipe[2]{-1, -1};
            if (pipe(output_pipe) != 0)
                throw std::runtime_error(
                    std::string("creating helper pipe failed: ") +
                    std::strerror(errno));
            Fd read_end(output_pipe[0]);
            Fd write_end(output_pipe[1]);

            child = spawn_command(allowed, read_end.get(), write_end.get());
            (void)close(write_end.release());
            bool stop_on_start = false;
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                active_pid_ = child;
                active_id_ = id;
                active_has_output_ = false;
                stop_on_start = pending_stop_ && !active_waits_for_output_;
                if (stop_on_start) pending_stop_ = false;
            }
            if (!emit(Json::Object{
                    {"type", Json("started")},
                    {"id", Json::integer(id)},
                    {"pid", Json::unsigned_integer(
                        static_cast<std::uint64_t>(child))},
                    {"pgid", Json::unsigned_integer(
                        static_cast<std::uint64_t>(child))}})) {
                throw std::runtime_error("GUI protocol output closed");
            }
            if (stop_on_start)
                deliver_stop(child, Json(nullptr), id);

            FILE* stream = fdopen(read_end.release(), "r");
            if (stream == nullptr)
                throw std::runtime_error(
                    std::string("fdopen failed: ") + std::strerror(errno));
            std::unique_ptr<FILE, int(*)(FILE*)> input(stream, std::fclose);

            static const std::regex progress_expression(
                R"(^\s*(\d+(?:\.\d+)?)\s+percent completed\s*$)",
                std::regex::ECMAScript | std::regex::icase);
            char* line = nullptr;
            std::size_t capacity = 0U;
            double last_progress = -1.0;
            auto last_progress_time =
                std::chrono::steady_clock::time_point::min();

            while (getline(&line, &capacity, stream) >= 0) {
                std::string clean(line);
                while (!clean.empty() &&
                       (clean.back() == '\n' || clean.back() == '\r')) {
                    clean.pop_back();
                }

                bool deliver_queued_stop = false;
                {
                    std::lock_guard<std::mutex> lock(active_mutex_);
                    if (!active_has_output_) {
                        active_has_output_ = true;
                        deliver_queued_stop = pending_stop_;
                        pending_stop_ = false;
                    }
                }

                std::smatch match;
                bool delivered = true;
                if (std::regex_match(clean, match, progress_expression)) {
                    double percent = std::stod(match[1].str());
                    percent = std::clamp(percent, 0.0, 100.0);
                    const auto now = std::chrono::steady_clock::now();
                    const bool timed =
                        last_progress_time ==
                            std::chrono::steady_clock::time_point::min() ||
                        now - last_progress_time >=
                            std::chrono::milliseconds(250);
                    if (last_progress < 0.0 ||
                        std::fabs(percent - last_progress) >= 0.05 ||
                        timed || percent >= 100.0) {
                        delivered = emit(Json::Object{
                            {"type", Json("progress")},
                            {"id", Json::integer(id)},
                            {"percent", Json::real(percent)}});
                        last_progress = percent;
                        last_progress_time = now;
                    }
                } else {
                    delivered = emit(Json::Object{
                        {"type", Json("output")},
                        {"id", Json::integer(id)},
                        {"line", Json(clean)}});
                }

                if (!delivered) {
                    std::free(line);
                    line = nullptr;
                    throw std::runtime_error("GUI protocol output closed");
                }
                if (deliver_queued_stop)
                    deliver_stop(child, Json(nullptr), id,
                                 "queued SIGINT delivered after engine initialisation");
            }
            std::free(line);

            const int status = wait_for_child(child);
            reaped = true;
            return_code = WIFEXITED(status)
                ? WEXITSTATUS(status)
                : WIFSIGNALED(status)
                    ? 128 + WTERMSIG(status) : 127;
        } catch (const std::exception& error) {
            if (child > 0 && !reaped) stop_and_reap(child);
            if (!transport_failed_.load(std::memory_order_acquire)) {
                (void)fail(Json::integer(id), error.what());
            }
        }

        {
            // Publish idle state before completion: the GUI may immediately
            // submit the verification scan when it receives "finished".
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_pid_ = -1;
            active_id_ = 0;
            active_has_output_ = false;
            active_waits_for_output_ = false;
            pending_stop_ = false;
            worker_running_ = false;
        }
        (void)emit(Json::Object{
            {"type", Json("finished")}, {"id", Json::integer(id)},
            {"returncode", Json::integer(return_code)}});
    }

    void deliver_stop(pid_t pid, Json id, std::int64_t active_id,
                      const char* success_message = "SIGINT delivered") {
        if (kill(-pid, SIGINT) == 0) {
            (void)emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", std::move(id)},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(true)},
                {"message", Json(success_message)}});
        } else {
            const int failure = errno;
            (void)emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", std::move(id)},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(false)},
                {"message", Json(
                    failure == ESRCH ? "operation already exited"
                                     : std::strerror(failure))}});
        }
    }

    void handle_stop(const Json& message) {
        pid_t pid = -1;
        std::int64_t active_id = 0;
        bool defer = false;
        bool running = false;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            pid = active_pid_;
            active_id = active_id_;
            running = worker_running_;
            // Writers need their cooperative handler installed. Read-only
            // analysis may be stopped before producing any map output.
            defer = running && (pid <= 0 ||
                (active_waits_for_output_ && !active_has_output_));
            if (defer) pending_stop_ = true;
        }

        const Json id = id_value(message);
        if (!running && pid <= 0) {
            (void)emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", id},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(false)},
                {"message", Json("no active operation")}});
            return;
        }
        if (defer) {
            (void)emit(Json::Object{
                {"type", Json("stop-result")},
                {"id", id},
                {"active_id", Json::integer(active_id)},
                {"delivered", Json(true)},
                {"message", Json(
                    "safe stop queued until engine initialisation")}});
            return;
        }
        deliver_stop(pid, id, active_id);
    }

    void stop_active_and_wait() {
        pid_t signalled = -1;
        for (;;) {
            pid_t pid = -1;
            bool running = false;
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                pid = active_pid_;
                running = worker_running_;
            }
            if (pid > 0 && pid != signalled) {
                if (kill(-pid, SIGINT) == 0 || errno == ESRCH)
                    signalled = pid;
            }
            if (!running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (worker_.joinable()) worker_.join();
    }
};

bool ignore_sigpipe() {
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) != 0) return false;
    return sigaction(SIGPIPE, &action, nullptr) == 0;
}

} // namespace
} // namespace defragger

int main() {
#ifndef LD_PRIVILEGED_HELPER_TEST_MODE
    if (geteuid() != 0) {
        std::fputs(
            "Defragmenter privileged helper must run as root\n", stderr);
        return 1;
    }
#endif
    if (!defragger::ignore_sigpipe()) {
        std::fprintf(
            stderr, "Defragmenter privileged helper: cannot ignore SIGPIPE: %s\n",
            std::strerror(errno));
        return 1;
    }
    try {
#ifdef LD_PRIVILEGED_HELPER_TEST_MODE
        constexpr std::uint32_t invoking_uid = 1000U;
#else
        const std::uint32_t invoking_uid =
            defragger::invoking_uid_from_environment();
#endif
        defragger::Helper helper(invoking_uid);
        return helper.run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Defragmenter privileged helper: %s\n",
                     error.what());
        return 1;
    }
}
