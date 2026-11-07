// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"

extern "C" {
#include "filesystem_module_host.h"
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
        "usage: defragmenter-operation-engine-cpp "
        "[--list-plugins|--list-installed-modules] OPERATION DEVICE --filesystem TYPE [worker options...]\n",
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
            std::printf("defragmenter-operation-engine-cpp %s\n", LD_VERSION);
            return 0;
        }
        if (token == "--list-plugins") {
            std::puts(defragger::registry_manifest_json().c_str());
            return 0;
        }
        if (token == "--list-installed-modules") {
            return ifs_filesystem_module_list_v2();
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
    const bool generic_module = backend == nullptr;
    std::string generic_filename;

    if (generic_module) {
        /* A newly installed filesystem is eligible without recompiling the
         * application only if it implements the standard V2 ABI. The trusted
         * module host independently verifies ownership, ABI, exact format ID
         * and required capabilities before executing any operation. */
        if (filesystem.empty() || filesystem.size() > 15U ||
            (operation != "defrag" && operation != "growth-defrag" &&
             operation != "recover")) {
            std::fprintf(stderr,
                         "defragmenter-operation-engine: unsupported generic "
                         "filesystem or operation\n");
            return 2;
        }
        for (unsigned char character : filesystem) {
            if (!((character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') ||
                  character == '_')) {
                std::fputs("invalid generic filesystem identifier\n", stderr);
                return 2;
            }
        }
        char module_file[64] = {};
        if (ifs_filesystem_module_find_v2(filesystem.c_str(), module_file,
                                           sizeof(module_file)) != 0) {
            std::fprintf(stderr,
                         "defragmenter-operation-engine: no installed trusted "
                         "Defrag V2 SO advertises %s\n", filesystem.c_str());
            return 2;
        }
        generic_filename = module_file;
    }

    const defragger::OperationSpec* specification =
        backend ? defragger::operation_for(*backend, operation) : nullptr;
    if (!generic_module && specification == nullptr) {
        std::fprintf(stderr,
                     "defragmenter-operation-engine: the %s plugin does not "
                     "implement %s\n",
                     backend->display_name.c_str(), operation.c_str());
        return 2;
    }

    struct stat status {};
    if (stat(device.c_str(), &status) != 0) {
        std::fprintf(stderr,
                     "defragmenter-operation-engine: cannot inspect target "
                     "%s: %s\n",
                     device.c_str(), std::strerror(errno));
        return 2;
    }
    if (!S_ISREG(status.st_mode) && !S_ISBLK(status.st_mode)) {
        std::fprintf(stderr,
                     "defragmenter-operation-engine: target is neither a "
                     "regular image nor a block device\n");
        return 2;
    }
    if (ld_path_is_mounted(device.c_str())) {
        std::fprintf(stderr,
                     "defragmenter-operation-engine: %s or overlapping "
                     "storage is mounted; unmount it before mutation\n",
                     device.c_str());
        return 2;
    }
    if (S_ISBLK(status.st_mode)) {
        bool system_use = true;
        if (ld_block_device_has_system_use(device.c_str(), &system_use) != 0) {
            std::fprintf(stderr,
                         "defragmenter-operation-engine: cannot prove %s "
                         "is free of active system use: %s\n",
                         device.c_str(), std::strerror(errno));
            return 2;
        }
        if (system_use) {
            std::fprintf(stderr,
                         "defragmenter-operation-engine: %s or related "
                         "storage is in active system use (including swap); "
                         "disable that use before mutation\n",
                         device.c_str());
            return 2;
        }
    }

    if (generic_module) {
        /* The same offline-device safety preflight above applies to every
         * future module. No format-specific parser or mutation code lives in
         * this dispatcher, and absent/incompatible SOs fail closed. */
        std::vector<std::string> command;
        command.reserve(3U + forwarded.size());
        command.emplace_back("defragmenter-generic-module");
        command.push_back(operation);
        command.push_back(device);
        command.insert(command.end(), forwarded.begin(), forwarded.end());
        std::vector<char*> arguments;
        arguments.reserve(command.size() + 1U);
        for (auto& element : command) arguments.push_back(element.data());
        arguments.push_back(nullptr);
        (void)setenv("LC_ALL", "C", 1);
        (void)setenv("LANG", "C", 1);
        return ifs_filesystem_module_run(
            filesystem.c_str(), generic_filename.c_str(),
            static_cast<int>(command.size()), arguments.data());
    }

    std::string worker;
    try {
        worker = defragger::resolve_program(specification->worker, false);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "defragmenter-operation-engine: %s\n",
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
                 "defragmenter-operation-engine: exec %s failed: %s\n",
                 worker.c_str(), std::strerror(errno));
    return 127;
}
