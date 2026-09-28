// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"

extern "C" {
#include "ld_device.h"
#include "version.h"
}

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int usage() {
    std::fputs(
        "usage: linux-defragger-operation-engine-cpp "
        "[--list-plugins] OPERATION DEVICE --filesystem TYPE [worker options...]\n",
        stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string operation;
    std::string device;
    std::string filesystem;
    std::vector<std::string> forwarded;

    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (token == "--version") {
            std::printf("linux-defragger-operation-engine-cpp %s\n", LD_VERSION);
            return 0;
        }
        if (token == "--list-plugins") {
            std::puts(defragger::registry_manifest_json().c_str());
            return 0;
        }
        if (token == "--filesystem") {
            if (++index >= argc) return usage();
            filesystem = argv[index];
            continue;
        }
        constexpr const char prefix[] = "--filesystem=";
        if (token.rfind(prefix, 0U) == 0U) {
            filesystem = token.substr(sizeof(prefix) - 1U);
            continue;
        }
        if (operation.empty() && (token.empty() || token[0] != '-')) {
            operation = token;
            continue;
        }
        if (device.empty() && (token.empty() || token[0] != '-')) {
            device = token;
            continue;
        }
        forwarded.push_back(token);
    }

    if (operation.empty() || device.empty() || filesystem.empty()) return usage();

    const defragger::BackendInfo* backend =
        defragger::backend_by_fstype(filesystem);
    if (backend == nullptr) {
        std::fprintf(stderr,
                     "linux-defragger-operation-engine: no filesystem plugin "
                     "is registered for %s\n",
                     filesystem.c_str());
        return 2;
    }
    const defragger::OperationSpec* specification =
        defragger::operation_for(*backend, operation);
    if (specification == nullptr) {
        std::fprintf(stderr,
                     "linux-defragger-operation-engine: the %s plugin does not "
                     "implement %s\n",
                     backend->display_name.c_str(), operation.c_str());
        return 2;
    }

    struct stat status {};
    if (stat(device.c_str(), &status) != 0) {
        std::fprintf(stderr,
                     "linux-defragger-operation-engine: cannot inspect target "
                     "%s: %s\n",
                     device.c_str(), std::strerror(errno));
        return 2;
    }
    if (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode)) {
        std::fprintf(stderr,
                     "linux-defragger-operation-engine: target is neither a "
                     "regular image nor a block device\n");
        return 2;
    }
    if (ld_path_is_mounted(device.c_str())) {
        std::fprintf(stderr,
                     "linux-defragger-operation-engine: %s or overlapping "
                     "storage is mounted; unmount it before mutation\n",
                     device.c_str());
        return 2;
    }
    if (S_ISBLK(status.st_mode)) {
        bool system_use = true;
        if (ld_block_device_has_system_use(device.c_str(), &system_use) != 0) {
            std::fprintf(stderr,
                         "linux-defragger-operation-engine: cannot prove %s "
                         "is free of active system use: %s\n",
                         device.c_str(), std::strerror(errno));
            return 2;
        }
        if (system_use) {
            std::fprintf(stderr,
                         "linux-defragger-operation-engine: %s or related "
                         "storage is in active system use (including swap); "
                         "disable that use before mutation\n",
                         device.c_str());
            return 2;
        }
    }

    std::string worker;
    try {
        worker = defragger::resolve_program(specification->worker);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "linux-defragger-operation-engine: %s\n",
                     error.what());
        return 2;
    }

    const auto options =
        defragger::without_options(forwarded, specification->unsupported_options);
    std::vector<std::string> command;
    command.reserve(3U + options.size());
    command.push_back(worker);
    command.push_back(operation);
    command.push_back(device);
    command.insert(command.end(), options.begin(), options.end());

    std::vector<char*> raw;
    raw.reserve(command.size() + 1U);
    for (auto& item : command) raw.push_back(item.data());
    raw.push_back(nullptr);

    (void)setenv("LC_ALL", "C", 1);
    (void)setenv("LANG", "C", 1);
    execv(raw[0], raw.data());
    std::fprintf(stderr,
                 "linux-defragger-operation-engine: exec %s failed: %s\n",
                 worker.c_str(), std::strerror(errno));
    return 127;
}
