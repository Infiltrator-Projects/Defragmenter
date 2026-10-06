// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_process_session.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace defragger {
namespace {

void append_bounded(std::string& text, std::string_view line)
{
    text.append(line);
    constexpr std::size_t kLimit = 4096U;
    if (text.size() > kLimit)
        text.erase(0U, text.size() - kLimit);
}

} // namespace

struct PrivilegedHelperTransport::State {
    std::string helper_path;
    LineCallback on_line;
    ClosedCallback on_closed;
    GSubprocess* process = nullptr;
    GDataInputStream* input = nullptr;
    bool shutting_down = false;
};

PrivilegedHelperTransport::PrivilegedHelperTransport(
    std::string helper_path,
    LineCallback on_line,
    ClosedCallback on_closed)
    : state_(std::make_shared<State>())
{
    state_->helper_path = std::move(helper_path);
    state_->on_line = std::move(on_line);
    state_->on_closed = std::move(on_closed);
}

PrivilegedHelperTransport::~PrivilegedHelperTransport()
{
    shutdown();
}

bool PrivilegedHelperTransport::active() const noexcept
{
    return state_ != nullptr && state_->process != nullptr;
}

void PrivilegedHelperTransport::start()
{
    if (active()) return;
    if (state_ == nullptr || state_->helper_path.empty())
        throw std::runtime_error("administrator helper path is unavailable");

    state_->shutting_down = false;
    const char* argv[] = {"pkexec", state_->helper_path.c_str(), nullptr};
    GError* failure = nullptr;
    state_->process = g_subprocess_newv(
        argv,
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE),
        &failure);
    if (state_->process == nullptr) {
        const std::string message =
            failure != nullptr ? failure->message : "pkexec unavailable";
        if (failure != nullptr) g_error_free(failure);
        throw std::runtime_error(message);
    }

    state_->input = g_data_input_stream_new(
        g_subprocess_get_stdout_pipe(state_->process));
    read_next(state_);
}

void PrivilegedHelperTransport::send(const Json& request)
{
    if (!active())
        throw std::runtime_error("Administrator helper is unavailable");

    const std::string line = request.dump() + "\n";
    GError* failure = nullptr;
    gsize written = 0U;
    if (!g_output_stream_write_all(
            g_subprocess_get_stdin_pipe(state_->process),
            line.data(), line.size(), &written, nullptr, &failure)) {
        const std::string message =
            failure != nullptr ? failure->message : "helper pipe closed";
        if (failure != nullptr) g_error_free(failure);
        throw std::runtime_error(message);
    }
}

void PrivilegedHelperTransport::shutdown() noexcept
{
    if (state_ == nullptr) return;
    state_->shutting_down = true;
    state_->on_line = {};
    state_->on_closed = {};
    if (state_->process != nullptr)
        g_subprocess_force_exit(state_->process);
    if (state_->input != nullptr) {
        g_object_unref(state_->input);
        state_->input = nullptr;
    }
    if (state_->process != nullptr) {
        g_object_unref(state_->process);
        state_->process = nullptr;
    }
}

void PrivilegedHelperTransport::read_next(
    const std::shared_ptr<State>& state)
{
    if (state == nullptr || state->input == nullptr || state->shutting_down)
        return;

    auto* context = new std::shared_ptr<State>(state);
    g_data_input_stream_read_line_async(
        state->input, G_PRIORITY_DEFAULT, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            const std::unique_ptr<std::shared_ptr<State>> context(
                static_cast<std::shared_ptr<State>*>(data));
            const auto state = *context;
            GError* failure = nullptr;
            gsize length = 0U;
            gchar* line = g_data_input_stream_read_line_finish(
                G_DATA_INPUT_STREAM(source), result, &length, &failure);

            if (line != nullptr) {
                const std::string value(line, length);
                g_free(line);
                if (!state->shutting_down && state->on_line)
                    state->on_line(value);
                PrivilegedHelperTransport::read_next(state);
                return;
            }

            const std::string detail =
                failure != nullptr
                    ? failure->message
                    : "administrator session ended";
            if (failure != nullptr) g_error_free(failure);

            if (state->input != nullptr) {
                g_object_unref(state->input);
                state->input = nullptr;
            }
            if (state->process != nullptr) {
                g_object_unref(state->process);
                state->process = nullptr;
            }
            if (!state->shutting_down && state->on_closed)
                state->on_closed(detail);
        },
        context);
}

struct LocalAnalysisTransport::State {
    LineCallback on_line;
    FinishedCallback on_finished;
    GSubprocess* process = nullptr;
    std::string detail;
    bool waited = false;
    bool shutting_down = false;
    int code = 127;
    unsigned int streams = 0U;
};

LocalAnalysisTransport::LocalAnalysisTransport(
    LineCallback on_line,
    FinishedCallback on_finished)
    : state_(std::make_shared<State>())
{
    state_->on_line = std::move(on_line);
    state_->on_finished = std::move(on_finished);
}

LocalAnalysisTransport::~LocalAnalysisTransport()
{
    shutdown();
}

bool LocalAnalysisTransport::active() const noexcept
{
    return state_ != nullptr && state_->process != nullptr;
}

void LocalAnalysisTransport::start(
    const std::vector<std::string>& arguments)
{
    if (active())
        throw std::runtime_error("local analysis process is already active");
    if (arguments.empty())
        throw std::runtime_error("local analysis command is empty");

    state_->detail.clear();
    state_->waited = false;
    state_->shutting_down = false;
    state_->code = 127;
    state_->streams = 0U;

    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);

    GError* failure = nullptr;
    auto* launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_set_child_setup(
        launcher,
        [](gpointer) {
            if (setpgid(0, 0) != 0) _exit(125);
        },
        nullptr, nullptr);
    state_->process = g_subprocess_launcher_spawnv(
        launcher, argv.data(), &failure);
    g_object_unref(launcher);

    if (state_->process == nullptr) {
        const std::string message =
            failure != nullptr ? failure->message : "Unable to start mapper";
        if (failure != nullptr) g_error_free(failure);
        throw std::runtime_error(message);
    }

    state_->streams = 2U;
    read_stream(
        state_,
        g_data_input_stream_new(
            g_subprocess_get_stdout_pipe(state_->process)),
        false);
    read_stream(
        state_,
        g_data_input_stream_new(
            g_subprocess_get_stderr_pipe(state_->process)),
        true);

    auto* context = new std::shared_ptr<State>(state_);
    g_subprocess_wait_async(
        state_->process, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            const std::unique_ptr<std::shared_ptr<State>> context(
                static_cast<std::shared_ptr<State>*>(data));
            const auto state = *context;
            GError* failure = nullptr;
            const bool waited = g_subprocess_wait_finish(
                G_SUBPROCESS(source), result, &failure);
            state->code =
                waited && g_subprocess_get_if_exited(G_SUBPROCESS(source))
                    ? g_subprocess_get_exit_status(G_SUBPROCESS(source))
                    : 127;
            if (failure != nullptr) {
                append_bounded(state->detail, failure->message);
                g_error_free(failure);
            }
            state->waited = true;
            LocalAnalysisTransport::finish_if_ready(state);
        },
        context);
}

void LocalAnalysisTransport::signal(int signal_number) noexcept
{
    if (!active()) return;
    const char* identifier = g_subprocess_get_identifier(state_->process);
    char* end = nullptr;
    const long pid = identifier != nullptr
        ? std::strtol(identifier, &end, 10)
        : 0L;
    if (pid > 0L && pid <= std::numeric_limits<pid_t>::max() &&
        end != nullptr && *end == '\0') {
        if (kill(-static_cast<pid_t>(pid), signal_number) == 0 ||
            errno != ESRCH) {
            return;
        }
    }
    g_subprocess_send_signal(state_->process, signal_number);
}

void LocalAnalysisTransport::shutdown() noexcept
{
    if (state_ == nullptr) return;
    state_->shutting_down = true;
    state_->on_line = {};
    state_->on_finished = {};
    if (state_->process != nullptr) {
        g_subprocess_force_exit(state_->process);
        g_object_unref(state_->process);
        state_->process = nullptr;
    }
}

void LocalAnalysisTransport::read_stream(
    const std::shared_ptr<State>& state,
    GDataInputStream* stream,
    bool diagnostics)
{
    struct ReadContext {
        std::shared_ptr<State> state;
        bool diagnostics;
    };
    auto* context = new ReadContext{state, diagnostics};
    g_data_input_stream_read_line_async(
        stream, G_PRIORITY_DEFAULT, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            const std::unique_ptr<ReadContext> context(
                static_cast<ReadContext*>(data));
            const auto state = context->state;
            GError* failure = nullptr;
            gsize length = 0U;
            char* raw = g_data_input_stream_read_line_finish(
                G_DATA_INPUT_STREAM(source), result, &length, &failure);

            if (raw != nullptr) {
                const std::string line(raw, length);
                g_free(raw);
                if (context->diagnostics) {
                    append_bounded(state->detail, line);
                    append_bounded(state->detail, "\n");
                }
                if (!state->shutting_down && state->on_line)
                    state->on_line(line, context->diagnostics);
                LocalAnalysisTransport::read_stream(
                    state, G_DATA_INPUT_STREAM(source), context->diagnostics);
                return;
            }

            if (failure != nullptr) {
                append_bounded(state->detail, failure->message);
                g_error_free(failure);
            }
            g_object_unref(source);
            if (state->streams != 0U) --state->streams;
            LocalAnalysisTransport::finish_if_ready(state);
        },
        context);
}

void LocalAnalysisTransport::finish_if_ready(
    const std::shared_ptr<State>& state)
{
    if (state == nullptr || !state->waited || state->streams != 0U ||
        state->process == nullptr) {
        return;
    }

    GSubprocess* process = state->process;
    state->process = nullptr;
    g_object_unref(process);

    if (!state->shutting_down && state->on_finished)
        state->on_finished(state->code, state->detail);
}

} // namespace defragger
