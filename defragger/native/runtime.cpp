// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
extern "C" {
#include "filesystem_module_host.h"
}

#include <infiltratr/escape.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace defragger {
namespace {

std::vector<std::string> worker_default_options(
    std::string_view worker, std::string_view operation_name) {
    (void)worker;
    (void)operation_name;
    // Buffering, relocation and recovery belong to the Common V2 module.
    return {};
}

OperationSpec operation(std::string name, std::string worker,
                        std::string warning = {}) {
    std::string policy;
    std::string label;
    std::string description;
    if (name == "defrag") {
        policy = "packed";
        label = "Defragment";
        description =
            "Invoke the installed filesystem provider's qualified packed "
            "relocation plan on the unmounted target.";
    } else if (name == "growth-defrag") {
        policy = "growth-10";
        label = "Growth Defrag";
        description =
            "Apply the same packed contiguous layout while reserving 10% of each "
            "regular file's allocated length immediately after that file.";
    } else if (name == "recover") {
        policy = "recovery";
        label = "Recover";
        description =
            "Invoke the selected Common V2 provider's durable recovery.";
    } else {
        throw std::logic_error("invalid operation declaration");
    }
    auto defaults = worker_default_options(worker, name);
    return {std::move(name), std::move(worker), std::move(policy),
            std::move(label), std::move(description), std::move(warning),
            std::move(defaults), {}, true, false};
}

std::string lower_ascii(std::string_view text) {
    std::string value(text);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

struct ProgramSpec {
    const char* id;
    const char* environment;
    const char* installed;
    const char* build_name;
};

constexpr std::array<ProgramSpec, 4> kPrograms{{
    {"mapper", "DEFRAGMENTER_MAPPER",
     "/System/Commands/Defragmenter/defragmenter-mapper", "defragmenter-mapper"},
    {"operation-engine", "DEFRAGMENTER_OPERATION_ENGINE",
     "/System/Commands/Defragmenter/defragmenter-operation-engine",
     "defragmenter-operation-engine"},
    {"helper", "DEFRAGMENTER_HELPER",
     "/System/Commands/Defragmenter/defragmenter-privileged-helper",
     "defragmenter-privileged-helper"},
    {"test-media", "DEFRAGMENTER_TEST_MEDIA",
     "/Programs/defragmenter-test-media",
     "defragmenter-test-media"},
}};

const ProgramSpec* program_spec(std::string_view id) {
    for (const auto& item : kPrograms) {
        if (id == item.id) return &item;
    }
    return nullptr;
}

fs::path executable_directory() {
    std::array<char, 4096> buffer{};
    const ssize_t length =
        readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (length <= 0) return {};
    buffer[static_cast<std::size_t>(length)] = '\0';
    return fs::path(buffer.data()).parent_path();
}

bool executable_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) &&
           access(path.c_str(), X_OK) == 0;
}

const char* qualification_mode_key(QualificationMode mode) {
    switch (mode) {
    case QualificationMode::None:
        return "none";
    case QualificationMode::WorkerPreflight:
        return "worker-preflight";
    case QualificationMode::MapOrWorkerPreflight:
        return "map-or-worker-preflight";
    }
    throw std::logic_error("unknown qualification mode");
}

 } // namespace

OperationSpec generic_operation(std::string_view name) {
    return operation(std::string(name),"operation-engine");
}

BackendInfo backend_from_provider(std::string id,std::uint64_t flags) {
    std::uint32_t caps=0u;
    std::vector<OperationSpec> ops;
    if(flags&IFS_FS_DEFRAG_MAP)caps|=CAP_ANALYSE|CAP_MAP;
    const auto write=IFS_FS_DEFRAG_PLAN|IFS_FS_DEFRAG_EXECUTE|IFS_FS_DEFRAG_VERIFY;
    if((flags&write)==write) {
        caps|=CAP_DEFRAG|CAP_GROWTH_DEFRAG;
        ops.push_back(generic_operation("defrag"));
        ops.push_back(generic_operation("growth-defrag"));
    }
    if(flags&IFS_FS_DEFRAG_RECOVER) {
        caps|=CAP_RECOVER;
        ops.push_back(generic_operation("recover"));
    }
    return {id,id,{},caps,"summary","operation-engine",
            std::move(ops),QualificationMode::None,{},{"summary","exact-allocation"}};
}

const std::vector<BackendInfo>& backend_registry() {
    // Inventory is a snapshot of installed trusted providers, not a
    // compile-time filesystem catalogue. Ambiguous identities are excluded.
    static const std::vector<BackendInfo> registry=[] {
        std::vector<BackendInfo> result;
        const int rc=ifs_filesystem_module_visit_v2(
            [](void *opaque,const char *id,std::uint64_t flags,std::uint64_t)->int {
                auto& items=*static_cast<std::vector<BackendInfo>*>(opaque);
                try {items.push_back(backend_from_provider(id,flags));}
                catch(...) {return -1;}
                return 0;
            },&result);
        if(rc)return std::vector<BackendInfo>{};
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        std::vector<BackendInfo> unique;
        for(std::size_t i=0;i<result.size();) {
            std::size_t next=i+1u;
            while(next<result.size() && result[next].id==result[i].id)++next;
            if(next==i+1u)unique.push_back(std::move(result[i]));
            i=next;
        }
        return unique;
    }();
    return registry;
}

const BackendInfo* backend_by_fstype(std::string_view filesystem) {
    const std::string key = lower_ascii(filesystem);
    for (const auto& backend : backend_registry()) {
        if (lower_ascii(backend.id) == key) return &backend;
        for (const auto& alias : backend.aliases) {
            if (lower_ascii(alias) == key) return &backend;
        }
    }
    return nullptr;
}

const OperationSpec* operation_for(const BackendInfo& backend,
                                   std::string_view operation_name) {
    for (const auto& item : backend.operations) {
        if (item.name == operation_name) return &item;
    }
    return nullptr;
}

bool backend_accepts_map_accuracy(const BackendInfo& backend,
                                  std::string_view accuracy) {
    if (backend.accepted_map_accuracies.empty())
        return accuracy == backend.map_accuracy;
    return std::find(
               backend.accepted_map_accuracies.begin(),
               backend.accepted_map_accuracies.end(),
               accuracy) != backend.accepted_map_accuracies.end();
}

std::vector<std::string> without_options(
    const std::vector<std::string>& arguments,
    const std::vector<std::string>& unsupported) {
    if (unsupported.empty()) return arguments;

    std::vector<std::string> filtered;
    for (std::size_t index = 0; index < arguments.size();) {
        const std::string& token = arguments[index];
        bool blocked = false;
        for (const auto& option : unsupported) {
            if (token == option ||
                (token.size() > option.size() &&
                 token.compare(0, option.size(), option) == 0 &&
                 token[option.size()] == '=')) {
                blocked = true;
                break;
            }
        }
        if (!blocked) {
            filtered.push_back(token);
            ++index;
            continue;
        }
        ++index;
        if (index < arguments.size() &&
            !arguments[index].empty() &&
            arguments[index][0] != '-') {
            ++index;
        }
    }
    return filtered;
}

std::string resolve_program(std::string_view program_id,
                            bool allow_environment_override) {
    const ProgramSpec* spec = program_spec(program_id);
    if (spec == nullptr)
        throw std::runtime_error(
            "unknown Defragmenter program: " + std::string(program_id));

    std::vector<fs::path> candidates;
    // Development overrides are an explicit caller capability. Privileged
    // dispatch passes false so root worker selection can never be influenced
    // by DEFRAGMENTER_* executable environment variables.
    if (allow_environment_override && program_id != "helper") {
        if (const char* override_path = std::getenv(spec->environment);
            override_path != nullptr && *override_path != '\0') {
            candidates.emplace_back(override_path);
        }
    }
    const fs::path directory = executable_directory();
    if (!directory.empty()) candidates.push_back(directory / spec->build_name);
    candidates.emplace_back(spec->installed);

    for (const auto& candidate : candidates) {
        if (executable_file(candidate)) return candidate.string();
    }

    std::string checked;
    for (const auto& candidate : candidates) {
        if (!checked.empty()) checked += ", ";
        checked += candidate.string();
    }
    throw std::runtime_error(
        "could not locate " + std::string(program_id) + "; checked: " + checked);
}

std::string json_quote(std::string_view value) {
    const std::string input(value);
    std::size_t required = 0U;
    if (!infiltratr_escape_json(input.c_str(), nullptr, 0U, &required) ||
        required == 0U) {
        throw std::runtime_error("JSON escaping measurement failed");
    }
    std::vector<char> escaped(required);
    if (!infiltratr_escape_json(
            input.c_str(), escaped.data(), escaped.size(), nullptr)) {
        throw std::runtime_error("JSON escaping failed");
    }
    return "\"" + std::string(escaped.data()) + "\"";
}

std::string registry_manifest_json(unsigned schema) {
    if (schema == 0U) throw std::invalid_argument("manifest schema must be non-zero");
    std::string out = "{\"schema\":" + std::to_string(schema) + ",\"backends\":[";
    bool first_backend = true;
    for (const auto& backend : backend_registry()) {
        if (!first_backend) out += ',';
        first_backend = false;
        out += "{\"id\":" + json_quote(backend.id);
        out += ",\"display_name\":" + json_quote(backend.display_name);
        out += ",\"aliases\":[";
        for (std::size_t index = 0; index < backend.aliases.size(); ++index) {
            if (index != 0U) out += ',';
            out += json_quote(backend.aliases[index]);
        }
        out += "],\"capabilities\":" + std::to_string(backend.capabilities);
        out += ",\"map_accuracy\":" + json_quote(backend.map_accuracy);
        out += ",\"accepted_map_accuracies\":[";
        if (backend.accepted_map_accuracies.empty()) {
            out += json_quote(backend.map_accuracy);
        } else {
            for (std::size_t index = 0;
                 index < backend.accepted_map_accuracies.size(); ++index) {
                if (index != 0U) out += ',';
                out += json_quote(backend.accepted_map_accuracies[index]);
            }
        }
        out += ']';
        out += ",\"qualification_mode\":" +
               json_quote(qualification_mode_key(backend.qualification_mode));
        out += ",\"map_auxiliary_worker\":" +
               json_quote(backend.map_auxiliary_worker);
        out += ",\"operations\":[";
        for (std::size_t index = 0; index < backend.operations.size(); ++index) {
            if (index != 0U) out += ',';
            const auto& op = backend.operations[index];
            out += "{\"name\":" + json_quote(op.name);
            out += ",\"worker\":" + json_quote(op.worker);
            out += ",\"layout_policy\":" + json_quote(op.layout_policy);
            out += ",\"label\":" + json_quote(op.label);
            out += ",\"description\":" + json_quote(op.description);
            out += ",\"warning\":" + json_quote(op.warning);
            out += ",\"default_options\":[";
            for (std::size_t option = 0; option < op.default_options.size();
                 ++option) {
                if (option != 0U) out += ',';
                out += json_quote(op.default_options[option]);
            }
            out += "]";
            out += ",\"unsupported_options\":[";
            for (std::size_t option = 0; option < op.unsupported_options.size();
                 ++option) {
                if (option != 0U) out += ',';
                out += json_quote(op.unsupported_options[option]);
            }
            out += "]";
            out += ",\"raw_offline\":";
            out += op.raw_offline ? "true" : "false";
            out += ",\"live_updates\":";
            out += op.live_updates ? "true" : "false";
            out += '}';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

} // namespace defragger