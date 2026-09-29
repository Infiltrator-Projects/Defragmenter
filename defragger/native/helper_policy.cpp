// SPDX-License-Identifier: GPL-3.0-or-later
#include "helper_policy.hpp"

#include <cstdlib>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace defragger {
namespace {

constexpr std::string_view kMapper =
    "/usr/lib/linux-defragger/linux-defragger-mapper";
constexpr std::string_view kOperationEngine =
    "/usr/lib/linux-defragger/linux-defragger-operation-engine";
constexpr std::string_view kUdisksctl = "/usr/bin/udisksctl";
constexpr std::string_view kStateRoot = "/var/lib/linux-defragger/state";

std::uint32_t parse_uid(const char* raw) {
    if (raw == nullptr || *raw == '\0')
        throw std::runtime_error("empty invoking UID");
    std::string_view value(raw);
    std::uint64_t parsed = 0U;
    for (const char character : value) {
        if (character < '0' || character > '9')
            throw std::runtime_error("invalid invoking UID");
        parsed = parsed * 10U +
                 static_cast<std::uint64_t>(character - '0');
        if (parsed > UINT32_MAX)
            throw std::runtime_error("invoking UID is out of range");
    }
    return static_cast<std::uint32_t>(parsed);
}

void validate_operation_args(const std::vector<std::string>& arguments,
                             std::uint32_t invoking_uid) {
    if (arguments.size() < 2U ||
        (arguments[0] != "defrag" &&
         arguments[0] != "growth-defrag" &&
         arguments[0] != "recover")) {
        throw std::runtime_error("operation-engine command is not allowed");
    }

    const std::string& operation = arguments[0];
    const std::string& device = arguments[1];
    if (device.empty() || device[0] != '/')
        throw std::runtime_error("operation target must be an absolute path");

    bool has_filesystem = false;
    bool has_write = false;
    bool has_confirm = false;
    bool has_growth = false;
    std::size_t journal_count = 0U;
    fs::path journal;

    for (std::size_t index = 2U; index < arguments.size();) {
        const std::string& token = arguments[index];
        if (token == "--write") {
            if (has_write)
                throw std::runtime_error("operation-engine request repeats --write");
            has_write = true;
            ++index;
            continue;
        }

        const bool takes_value =
            token == "--filesystem" || token == "--confirm" ||
            token == "--journal" || token == "--growth-percent" ||
            token == "--batch-clusters" || token == "--ram-buffer" ||
            token == "--live-map-cells";
        if (!takes_value)
            throw std::runtime_error(
                "operation-engine request contains an unsupported option: " + token);
        if (index + 1U >= arguments.size())
            throw std::runtime_error(
                "operation-engine request has a truncated option: " + token);
        const std::string& value = arguments[index + 1U];
        if (value.empty())
            throw std::runtime_error(
                "operation-engine request has an empty option value: " + token);

        if (token == "--filesystem") {
            if (has_filesystem)
                throw std::runtime_error("operation-engine request repeats --filesystem");
            static const std::regex filesystem_name(
                R"(^[A-Za-z0-9+_.-]+$)", std::regex::ECMAScript);
            if (!std::regex_match(value, filesystem_name))
                throw std::runtime_error("operation filesystem identifier is not valid");
            has_filesystem = true;
        } else if (token == "--confirm") {
            if (has_confirm)
                throw std::runtime_error("operation-engine request repeats --confirm");
            if (value != device)
                throw std::runtime_error("operation confirmation does not match target");
            has_confirm = true;
        } else if (token == "--journal") {
            ++journal_count;
            journal = value;
        } else if (token == "--growth-percent") {
            if (has_growth || operation != "growth-defrag" || value != "10")
                throw std::runtime_error(
                    "growth percentage is permitted only once as 10 for growth-defrag");
            has_growth = true;
        }
        index += 2U;
    }

    if (!has_filesystem || !has_write || !has_confirm)
        throw std::runtime_error(
            "operation-engine request is missing required mutation authority");
    if (operation == "growth-defrag" && !has_growth)
        throw std::runtime_error(
            "growth-defrag request must provide the fixed 10 percent reserve");
    if (journal_count != 1U)
        throw std::runtime_error(
            "operation-engine request must provide exactly one journal");

    const fs::path expected_parent =
        fs::path(kStateRoot) / std::to_string(invoking_uid);
    if (!journal.is_absolute() ||
        journal.lexically_normal().parent_path() != expected_parent) {
        throw std::runtime_error(
            "operation journal must be directly below " +
            expected_parent.string());
    }
    static const std::regex filename(
        R"(^[A-Za-z0-9_.-]+\.journal$)",
        std::regex::ECMAScript);
    if (!std::regex_match(journal.filename().string(), filename))
        throw std::runtime_error("operation journal filename is not valid");
}

} // namespace

std::uint32_t invoking_uid_from_environment() {
    for (const char* name : {"PKEXEC_UID", "SUDO_UID"}) {
        const char* raw = std::getenv(name);
        if (raw != nullptr && *raw != '\0') return parse_uid(raw);
    }
    if (geteuid() != 0)
        return static_cast<std::uint32_t>(getuid());
    throw std::runtime_error(
        "cannot determine the unprivileged caller UID");
}

HelperCommand helper_command(
    std::string_view program,
    const std::vector<std::string>& arguments,
    std::uint32_t invoking_uid) {
    HelperCommand command;
    if (program == "operation-engine") {
        validate_operation_args(arguments, invoking_uid);
        command.executable = std::string(kOperationEngine);
    } else if (program == "mapper") {
        command.executable = std::string(kMapper);
    } else if (program == "udisksctl") {
        if (arguments.size() != 3U ||
            arguments[0] != "unmount" ||
            arguments[1] != "-b" ||
            arguments[2].rfind("/dev/", 0U) != 0U) {
            throw std::runtime_error(
                "only udisksctl unmount -b /dev/... is allowed");
        }
        command.executable = std::string(kUdisksctl);
    } else {
        throw std::runtime_error("unknown helper program");
    }
    command.arguments = arguments;
    return command;
}

} // namespace defragger
