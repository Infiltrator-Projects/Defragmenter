// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.hpp"
#include "map.hpp"
#include "runtime.hpp"

extern "C" {
#include "version.h"
}

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

int usage() {
    std::fputs(
        "usage: linux-defragger-mapper-cpp "
        "[PATH] [--fstype TYPE] [--cells N] [--probe] [--list-backends]\n",
        stderr);
    return 2;
}

bool parse_cell_count(std::string_view text, std::size_t& cells) {
    if (text.empty() || text.front() == '-' || text.front() == '+')
        return false;

    unsigned long long value = 0U;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0U ||
        value > static_cast<unsigned long long>(defragger::kMaxMapCells)) {
        return false;
    }
    cells = static_cast<std::size_t>(value);
    return true;
}

defragger::Json probe_result(
    const defragger::BackendInfo& backend,
    std::string_view identified_filesystem) {
    using defragger::Json;
    Json manifest = Json::parse(defragger::registry_manifest_json(2U));
    const Json& backends = manifest.at("backends");
    Json operations(Json::Array{});
    for (const auto& entry : backends.array()) {
        if (entry.at("id").string_or() == backend.id) {
            operations = entry.at("operations");
            break;
        }
    }
    Json::Object out;
    out["filesystem"] = Json(std::string(identified_filesystem));
    out["backend_id"] = Json(backend.id);
    out["capabilities"] = Json::unsigned_integer(backend.capabilities);
    out["map_accuracy"] = Json(backend.map_accuracy);
    out["operations"] = std::move(operations);
    return Json(std::move(out));
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string filesystem;
    std::size_t cells = 4096U;
    bool list = false;
    bool probe = false;

    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (token == "--version") {
            std::printf("linux-defragger-mapper-cpp %s\n", LD_VERSION);
            return 0;
        }
        if (token == "--list-backends") {
            list = true;
            continue;
        }
        if (token == "--probe") {
            probe = true;
            continue;
        }
        if (token == "--fstype") {
            if (++index >= argc) return usage();
            filesystem = argv[index];
            if (filesystem.empty()) return usage();
            continue;
        }
        constexpr std::string_view fstype_prefix = "--fstype=";
        if (token.rfind(fstype_prefix, 0U) == 0U) {
            filesystem = token.substr(fstype_prefix.size());
            if (filesystem.empty()) return usage();
            continue;
        }
        if (token == "--cells") {
            if (++index >= argc ||
                !parse_cell_count(argv[index], cells)) {
                return usage();
            }
            continue;
        }
        constexpr std::string_view cells_prefix = "--cells=";
        if (token.rfind(cells_prefix, 0U) == 0U) {
            if (!parse_cell_count(
                    std::string_view(token).substr(cells_prefix.size()),
                    cells)) {
                return usage();
            }
            continue;
        }
        if (!token.empty() && token[0] == '-') return usage();
        if (!path.empty()) return usage();
        path = token;
    }

    if (list) {
        std::puts(defragger::registry_manifest_json(2U).c_str());
        return 0;
    }
    if (path.empty()) return usage();

    try {
        const defragger::BackendInfo* backend = nullptr;
        std::string identified_filesystem;
        if (filesystem == "vfat" || filesystem == "fat" || filesystem == "msdos") {
            // Linux's generic FAT type does not encode the allocation width.
            // Probe authoritative geometry rather than guessing FAT32.
            for (const auto* id : {"fat12", "fat16", "fat32"}) {
                const auto* candidate = defragger::backend_by_fstype(id);
                if (candidate == nullptr) continue;
                const std::string identified =
                    defragger::backend_identified_filesystem(*candidate, path);
                if (!identified.empty()) {
                    backend = candidate;
                    identified_filesystem = identified;
                    break;
                }
            }
        } else if (!filesystem.empty()) {
            backend = defragger::backend_by_fstype(filesystem);
            if (probe && backend != nullptr) {
                identified_filesystem =
                    defragger::backend_identified_filesystem(*backend, path);
                if (identified_filesystem.empty()) backend = nullptr;
            }
        } else {
            for (const auto& candidate : defragger::backend_registry()) {
                const std::string identified =
                    defragger::backend_identified_filesystem(candidate, path);
                if (!identified.empty()) {
                    backend = &candidate;
                    identified_filesystem = identified;
                    break;
                }
            }
        }
        if (backend == nullptr) {
            std::fputs(
                "No self-contained filesystem plugin recognised this volume.\n",
                stderr);
            return 2;
        }
        if (probe) {
            if (identified_filesystem.empty()) {
                identified_filesystem =
                    defragger::backend_identified_filesystem(*backend, path);
            }
            if (identified_filesystem.empty()) {
                std::fputs(
                    "The selected backend did not recognise this volume.\n",
                    stderr);
                return 2;
            }
            std::puts(
                probe_result(*backend, identified_filesystem).dump().c_str());
            return 0;
        }
        defragger::Json result =
            defragger::map_backend(*backend, path, std::max<std::size_t>(1U, cells));
        if (!result.is_object())
            throw std::runtime_error("allocation mapper produced non-object result");
        result.object()["capabilities"] =
            defragger::Json::unsigned_integer(backend->capabilities);
        result.object()["backend_id"] = defragger::Json(backend->id);
        std::puts(result.dump().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Defragmenter mapper: %s\n", error.what());
        return 1;
    }
}
