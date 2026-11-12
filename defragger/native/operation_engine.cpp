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

    // Every filesystem, including built-in names, uses the one strict Common
    // V2 module contract. Known aliases are naming conveniences only; they
    // must never select the retired format-specific worker executables.
    const defragger::BackendInfo* backend =
        defragger::backend_by_fstype(filesystem);
    if (backend && backend->id == "ufs") {
        // Explicit UFS1/UFS2 stay exact. Other UFS aliases ask both
        // independent providers to probe the medium with Common V2.
        if (filesystem != "ufs1" && filesystem != "ufs2")
            filesystem = "ufs";
    } else if (backend && backend->id != "affs") {
        filesystem = backend->id;
    }
    if (operation != "defrag" && operation != "growth-defrag" &&
        operation != "recover" && operation != "analyse" &&
        operation != "analyse-json" && operation != "map" &&
        operation != "volume-info") {
        return usage();
    }
    if (filesystem.empty() || filesystem.size() > 15U) {
        std::fputs("invalid generic filesystem identifier\n", stderr);
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

    char module_file[64] = {};
    if (filesystem == "ufs") {
        char discovered[16] = {};
        if (ifs_filesystem_module_detect_ufs_v2(
                device.c_str(), discovered, sizeof(discovered),
                module_file, sizeof(module_file)) != 0) {
            std::fputs("defragmenter-operation-engine: unable to identify "
                       "a unique trusted UFS1/UFS2 Common V2 provider\n", stderr);
            return 2;
        }
        filesystem = discovered;
    }
    const bool readonly_volume_query = operation == "volume-info";
    const int lookup_result = readonly_volume_query
        ? ifs_filesystem_module_find_readonly_v2(
              filesystem.c_str(), module_file, sizeof(module_file))
        : ifs_filesystem_module_find_v2(
              filesystem.c_str(), module_file, sizeof(module_file));
    if (lookup_result != 0) {
        std::fprintf(stderr,
                     "defragmenter-operation-engine: no installed trusted "
                     "Common V2 SO qualifies for %s\n", filesystem.c_str());
        return 2;
    }
    const std::string generic_filename(module_file);

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
