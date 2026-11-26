// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.hpp"
#include "map.hpp"
#include "runtime.hpp"
extern "C" {
#include "ld_stop.h"
#include "filesystem_module_host.h"
}

extern "C" {
#include "version.h"
}

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

int usage() {
    std::fputs(
        "usage: defragmenter-mapper-cpp "
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

std::uint32_t provider_capabilities(const defragger::BackendInfo& backend,
                                    const std::string& format) {
    uint64_t flags=0;
    if(ifs_filesystem_module_capabilities_v2(format.c_str(),&flags)!=0)
        throw std::runtime_error("No unique installed Common V2 descriptor");
    std::uint32_t caps=0;
    if(flags&IFS_FS_DEFRAG_MAP)caps|=defragger::CAP_ANALYSE|defragger::CAP_MAP;
    const uint64_t write=IFS_FS_DEFRAG_PLAN|IFS_FS_DEFRAG_EXECUTE|IFS_FS_DEFRAG_VERIFY;
    if((flags&write)==write)caps|=defragger::CAP_DEFRAG|defragger::CAP_GROWTH_DEFRAG;
    if(flags&IFS_FS_DEFRAG_RECOVER)caps|=defragger::CAP_RECOVER;
    // Application policy can further restrict a provider, never extend it.
    (void)backend;
    return caps;
}

defragger::Json probe_result(
    const defragger::BackendInfo& backend,
    std::string_view identified_filesystem) {
    using defragger::Json;
    Json::Array operations;
    for(const auto& spec:backend.operations) {
        Json::Object item;
        item["name"]=Json(spec.name);
        item["label"]=Json(spec.label);
        operations.emplace_back(std::move(item));
    }
    Json::Object out;
    out["filesystem"] = Json(std::string(identified_filesystem));
    out["backend_id"] = Json(backend.id);
    const auto caps=provider_capabilities(backend,std::string(identified_filesystem));
    out["capabilities"] = Json::unsigned_integer(caps);
    out["map_accuracy"] = Json("unverified");
    Json::Array supported;
    for(const auto& item:operations) {
        const auto name=item.at("name").string_or();
        if((name=="defrag" && (caps&defragger::CAP_DEFRAG)) ||
           (name=="growth-defrag" && (caps&defragger::CAP_GROWTH_DEFRAG)) ||
           (name=="recover" && (caps&defragger::CAP_RECOVER)))
            supported.push_back(item);
    }
    out["operations"] = Json(std::move(supported));
    return Json(std::move(out));
}

} // namespace

int main(int argc, char** argv) {
    ld_stop_clear();
    ld_stop_install_handlers();
    std::string path;
    std::string filesystem;
    std::size_t cells = 4096U;
    bool list = false;
    bool probe = false;

    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (token == "--version") {
            std::printf("defragmenter-mapper-cpp %s\n", LD_VERSION);
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
        char format[IFS_FS_MAX_FORMAT_ID+1u]={},module[64]={};
        std::transform(filesystem.begin(),filesystem.end(),filesystem.begin(),
            [](unsigned char c){return static_cast<char>(std::tolower(c));});
        int rc=0;
        if(filesystem.empty() || filesystem=="auto")
            rc=ifs_filesystem_module_detect_installed_v2(path.c_str(),format,sizeof(format),module,sizeof(module));
        else {
            const char* requested[]={filesystem.c_str()};
            rc=ifs_filesystem_module_detect_v2(path.c_str(),requested,1u,format,sizeof(format),module,sizeof(module));
        }
        if(rc)throw std::runtime_error("No unique installed Common V2 provider recognises this target");
        std::string identified_filesystem(format);
        std::uint64_t flags=0;
        if(ifs_filesystem_module_capabilities_v2(format,&flags)!=0)
            throw std::runtime_error("Provider changed during discovery");
        const auto selected=defragger::backend_from_provider(format,flags);
        const auto* backend=&selected;
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
        // SWAP.so provides Common V2 probing and formatting, but no
        // allocation map or DEFRAG group. Never advertise a fabricated map.
        const auto caps=provider_capabilities(*backend,identified_filesystem);
        if ((caps & defragger::CAP_MAP) == 0U) {
            std::fputs("Defragmenter mapper: allocation mapping is unavailable for this filesystem\n", stderr);
            return 2;
        }
        defragger::Json result =
            defragger::map_backend(*backend, path, std::max<std::size_t>(1U, cells),identified_filesystem);
        if (!result.is_object())
            throw std::runtime_error("allocation mapper produced non-object result");
        result.object()["capabilities"] =
            defragger::Json::unsigned_integer(caps);
        result.object()["backend_id"] = defragger::Json(backend->id);
        std::puts(result.dump().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Defragmenter mapper: %s\n", error.what());
        return 1;
    }
}
