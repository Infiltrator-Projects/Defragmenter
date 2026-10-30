// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"

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
    if (operation_name == "recover") return {};
    if (worker == "fat-native" || worker == "exfat-native") {
        return {"--batch-clusters", "4096", "--ram-buffer", "auto"};
    }
    if (worker == "ext-native") {
        return {"--batch-clusters", "4096"};
    }
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
            "Rewrite the unmounted filesystem directly so files and directories "
            "are contiguous and every relocatable allocation is packed into the "
            "earliest legal allocation units.";
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
            "Complete or roll back an interrupted raw-device journal transaction.";
    } else {
        throw std::logic_error("invalid operation declaration");
    }
    auto defaults = worker_default_options(worker, name);
    return {std::move(name), std::move(worker), std::move(policy),
            std::move(label), std::move(description), std::move(warning),
            std::move(defaults), {}, true, true};
}

std::vector<OperationSpec> standard_write_ops(
    const char* worker,
    const char* defrag_warning = "",
    const char* growth_warning = "") {
    return {
        operation("defrag", worker, defrag_warning),
        operation("growth-defrag", worker, growth_warning),
        operation("recover", worker),
    };
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

constexpr std::array<ProgramSpec, 21> kPrograms{{
    {"hfsplus-native", "DEFRAGMENTER_HFSPLUS_WORKER",
     "/System/Commands/Defragmenter/hfsplus/defragmenter-hfsplus-worker",
     "defragmenter-hfsplus-worker"},
    {"hfs-native", "DEFRAGMENTER_HFS_ANALYSER",
     "/System/Commands/Defragmenter/hfs/hfs_analyser", "hfs_analyser"},
    {"affs-native", "DEFRAGMENTER_AFFS_WORKER",
     "/System/Commands/Defragmenter/affs/defragmenter-affs-worker",
     "defragmenter-affs-worker"},
    {"btrfs-native", "DEFRAGMENTER_BTRFS_WORKER",
     "/System/Commands/Defragmenter/btrfs/defragmenter-btrfs-worker",
     "defragmenter-btrfs-worker"},
    {"fat-native", "DEFRAGMENTER_FAT_WORKER",
     "/System/Commands/Defragmenter/fat/defragmenter-fat-worker",
     "defragmenter-fat-worker"},
    {"exfat-native", "DEFRAGMENTER_EXFAT_WORKER",
     "/System/Commands/Defragmenter/exfat/defragmenter-exfat-worker",
     "defragmenter-exfat-worker"},
    {"ntfs-native", "DEFRAGMENTER_NTFS_WORKER",
     "/System/Commands/Defragmenter/ntfs/defragmenter-ntfs-worker",
     "defragmenter-ntfs-worker"},
    {"ext-native", "DEFRAGMENTER_EXT_WORKER",
     "/System/Commands/Defragmenter/ext4/defragmenter-ext-worker",
     "defragmenter-ext-worker"},
    {"ext-metadata", "DEFRAGMENTER_EXT_METADATA_WORKER",
     "/System/Commands/Defragmenter/ext4/ext-metadata-worker",
     "ext-metadata-worker"},
    {"xfs-native", "DEFRAGMENTER_XFS_WORKER",
     "/System/Commands/Defragmenter/xfs/defragmenter-xfs-worker",
     "defragmenter-xfs-worker"},
    {"apfs-native", "DEFRAGMENTER_APFS_WORKER",
     "/System/Commands/Defragmenter/apfs/defragmenter-apfs-worker",
     "defragmenter-apfs-worker"},
    {"minix-native", "DEFRAGMENTER_MINIX_WORKER",
     "/System/Commands/Defragmenter/minix/defragmenter-minix-worker",
     "defragmenter-minix-worker"},
    {"sfs-native", "DEFRAGMENTER_SFS_WORKER",
     "/System/Commands/Defragmenter/sfs/defragmenter-sfs-worker",
     "defragmenter-sfs-worker"},
    {"pfs3-native", "DEFRAGMENTER_PFS3_WORKER",
     "/System/Commands/Defragmenter/pfs3/defragmenter-pfs3-worker",
     "defragmenter-pfs3-worker"},
    {"swap-native", "DEFRAGMENTER_SWAP_WORKER",
     "/System/Commands/Defragmenter/swap/defragmenter-swap-worker",
     "defragmenter-swap-worker"},
    {"ufs-native", "DEFRAGMENTER_UFS_WORKER",
     "/System/Commands/Defragmenter/ufs/defragmenter-ufs-worker",
     "defragmenter-ufs-worker"},
    {"zfs-native", "DEFRAGMENTER_ZFS_WORKER",
     "/System/Commands/Defragmenter/zfs/defragmenter-zfs-worker",
     "defragmenter-zfs-worker"},
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

void validate_backend_registry(const std::vector<BackendInfo>& registry) {
    std::vector<std::string> names;
    for (const auto& backend : registry) {
        if (backend.id.empty())
            throw std::logic_error("backend registry contains an empty id");
        if (backend.map_accuracy.empty())
            throw std::logic_error("backend " + backend.id + " has no map accuracy contract");
        if (program_spec(backend.worker) == nullptr)
            throw std::logic_error(
                "backend " + backend.id + " names an unknown worker: " +
                backend.worker);
        if (!backend.map_auxiliary_worker.empty() &&
            program_spec(backend.map_auxiliary_worker) == nullptr) {
            throw std::logic_error(
                "backend " + backend.id + " names an unknown map auxiliary worker: " +
                backend.map_auxiliary_worker);
        }

        std::vector<std::string> accuracies;
        for (const auto& accuracy : backend.accepted_map_accuracies) {
            if (accuracy.empty())
                throw std::logic_error(
                    "backend " + backend.id + " contains an empty accepted map accuracy");
            if (std::find(accuracies.begin(), accuracies.end(), accuracy) !=
                accuracies.end()) {
                throw std::logic_error(
                    "backend " + backend.id + " repeats accepted map accuracy: " + accuracy);
            }
            accuracies.push_back(accuracy);
        }

        const bool writable = (backend.capabilities & CAP_DEFRAG) != 0U;
        if (writable != !backend.operations.empty()) {
            throw std::logic_error(
                "backend " + backend.id +
                " has inconsistent mutation capabilities and operations");
        }
        if (!writable && backend.qualification_mode != QualificationMode::None) {
            throw std::logic_error(
                "read-only backend " + backend.id +
                " declares mutation qualification policy");
        }

        const std::string primary_name = lower_ascii(backend.id);
        if (std::find(names.begin(), names.end(), primary_name) != names.end())
            throw std::logic_error(
                "backend registry contains duplicate id: " + primary_name);
        names.push_back(primary_name);
        for (const auto& raw_alias : backend.aliases) {
            if (raw_alias.empty())
                throw std::logic_error(
                    "backend " + backend.id + " contains an empty alias");
            const std::string alias = lower_ascii(raw_alias);
            if (alias == primary_name) continue;
            if (std::find(names.begin(), names.end(), alias) != names.end())
                throw std::logic_error(
                    "backend registry contains colliding alias: " + alias);
            names.push_back(alias);
        }

        for (const auto& operation : backend.operations) {
            if (program_spec(operation.worker) == nullptr) {
                throw std::logic_error(
                    "backend " + backend.id + " operation " + operation.name +
                    " names an unknown worker: " + operation.worker);
            }
            for (const auto& option : operation.default_options) {
                if (option.empty())
                    throw std::logic_error(
                        "backend " + backend.id + " operation " + operation.name +
                        " contains an empty default option");
            }
        }
    }
}

} // namespace

const std::vector<BackendInfo>& backend_registry() {
    static const std::vector<BackendInfo> registry = [] {
        const std::uint32_t read = CAP_ANALYSE | CAP_MAP;
        const std::uint32_t write =
            read | CAP_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP |
            CAP_GROWTH_DEFRAG;

        std::vector<BackendInfo> result;
        result.reserve(18);
        result.push_back({
            "affs", "Amiga OFS/FFS",
            {"affs", "amiga", "ofs", "ffs", "dostype"}, write, "exact",
            "affs-native", MapAdapter::Affs,
            standard_write_ops(
                "affs-native",
                "Amiga OFS/FFS writing runs in the independently installed OFS.so or FFS.so; Defragmenter hosts the verified module only.",
                "Amiga Growth Defrag runs in OFS.so or FFS.so and reserves 10% free blocks after each supported regular file.")});
        result.push_back({
            "apfs", "Apple APFS", {"apfs"}, write, "exact-bounded-spaceman",
            "apfs-native", MapAdapter::NativeMap,
            standard_write_ops(
                "apfs-native",
                "APFS writing is offline and fail-closed to the qualified one-CIB, unencrypted, snapshot-free flat-tree subset; validated older descriptor-ring checkpoint history is retained unchanged.",
                "APFS Growth Defrag leaves an exact 10% free-block reserve after each supported regular-file data stream."),
            QualificationMode::WorkerPreflight});
        result.push_back({
            "btrfs", "Btrfs", {"btrfs"}, write, "exact",
            "btrfs-native", MapAdapter::NativeMap,
            standard_write_ops(
                "btrfs-native",
                "Btrfs writing is offline and fail-closed to the qualified single-device, CRC32C, level-0 mixed data/metadata subset, including relocation and verification of ordinary checksum-tree-protected regular-file extents.",
                "Btrfs Growth Defrag leaves an exact 10% free-sector reserve after each supported regular file."),
            QualificationMode::WorkerPreflight, {}, {"exact-single-device"}});
        result.push_back({
            "exfat", "exFAT", {"exfat"}, write, "exact",
            "exfat-native", MapAdapter::Exfat,
            standard_write_ops(
                "exfat-native",
                "exFAT writing is dispatched to the independently installed exFAT.so; the application has no private exFAT parser, allocator or journal.",
                "exFAT Growth Defrag runs in exFAT.so and reserves 10% free clusters after each regular file.")});
        result.push_back({
            "ext4", "ext2/3/4", {"ext2", "ext3", "ext4"}, write, "exact",
            "ext-native", MapAdapter::Ext, standard_write_ops("ext-native"),
            QualificationMode::WorkerPreflight, "ext-metadata"});
        for (const int bits : {12, 16, 32}) {
            const std::string id = "fat" + std::to_string(bits);
            result.push_back({
                id, "FAT" + std::to_string(bits),
                {id, bits == 32 ? "vfat" : "msdos" + std::to_string(bits)},
                write, "exact", "fat-native", MapAdapter::Fat,
                standard_write_ops("fat-native")});
        }
        result.push_back({
            "hfs", "Apple HFS", {"hfs"}, write, "exact",
            "hfs-native", MapAdapter::NativeMap,
            standard_write_ops(
                "hfs-native",
                "Classic HFS writing runs in HFS.so and relocates supported regular-file forks while preserving validated B-tree topology.",
                "Classic HFS Growth Defrag leaves a 10% free allocation-block reserve after each supported non-empty file fork."),
            QualificationMode::WorkerPreflight});
        result.push_back({
            "hfsplus", "Apple HFS+/HFSX", {"hfsplus", "hfs+", "hfsx"},
            write, "exact", "hfsplus-native", MapAdapter::HfsPlus,
            standard_write_ops(
                "hfsplus-native",
                "HFS+/HFSX writing runs in the independently installed HFSPLUS.so module.",
                "HFS+/HFSX Growth Defrag runs in HFSPLUS.so and reserves 10% free allocation blocks after each supported fork."),
            QualificationMode::WorkerPreflight});
        result.push_back({
            "minix", "Minix Filesystem", {"minix", "minix2", "minix3"},
            write, "exact", "minix-native", MapAdapter::NativeMap,
            standard_write_ops(
                "minix-native",
                "Minix filesystem writing runs in the independently installed MINIX.so module.",
                "Minix Growth Defrag leaves an exact 10% free-zone reserve after every regular file.")});
        result.push_back({
            "ntfs", "NTFS", {"ntfs", "ntfs3"}, write, "exact",
            "ntfs-native", MapAdapter::Ntfs,
            standard_write_ops(
                "ntfs-native",
                "NTFS writing runs in NTFS.so using offline raw I/O. No kernel filesystem mount, filesystem ioctl or external repair utility is used.",
                "NTFS Growth Defrag runs in NTFS.so and reserves 10% free clusters after each supported regular-file stream."),
            QualificationMode::MapOrWorkerPreflight});
        result.push_back({
            "pfs3", "Amiga PFS3", {"pfs3", "pfs", "professionalfilesystem"},
            write, "exact-allocation", "pfs3-native", MapAdapter::NativeMap,
            standard_write_ops(
                "pfs3-native",
                "Amiga PFS3 writing runs in PFS3.so, validates supported directories and links, and fails closed on unsupported entries or layouts.",
                "PFS3 Growth Defrag leaves an exact 10% free-block reserve after every supported regular file.")});
        result.push_back({
            "sfs", "Amiga SFS", {"sfs", "sfs0", "sfs2", "smartfilesystem"},
            write, "exact-allocation", "sfs-native", MapAdapter::NativeMap,
            standard_write_ops(
                "sfs-native",
                "Amiga SFS0/SFS2 writing runs in the independently installed SFS.so module.",
                "SFS0/SFS2 Growth Defrag leaves an exact 10% free-block reserve after every regular file.")});
        result.push_back({
            "swap", "Linux Swap", {"swap", "swapspace", "linux-swap"},
            read, "summary", "swap-native", MapAdapter::NativeMap, {},
            QualificationMode::None, {}, {"summary", "exact"}});
        result.push_back({
            "ufs", "Solaris/BSD UFS", {"ufs", "ufs1", "ufs2", "4.2bsd"},
            write, "exact", "ufs-native", MapAdapter::NativeMap,
            standard_write_ops(
                "ufs-native",
                "UFS1/UFS2 writing runs in its matching UFS1.so or UFS2.so and fails closed on unsupported filesystem states or allocation layouts.",
                "UFS Growth Defrag runs in the selected UFS.so engine and reserves 10% free fragments after each supported regular file."),
            QualificationMode::WorkerPreflight, {},
            {"summary", "exact-allocation", "exact"}});
        result.push_back({
            "xfs", "XFS", {"xfs"}, write, "exact",
            "xfs-native", MapAdapter::Xfs,
            standard_write_ops(
                "xfs-native",
                "XFS writing runs in XFS.so using offline raw I/O. No kernel filesystem mount, filesystem ioctl or external repair utility is used.",
                "XFS Growth Defrag runs in XFS.so, requires staging space, and reserves 10% free extents after each supported regular file."),
            QualificationMode::WorkerPreflight});
        result.push_back({
            "zfs", "ZFS/OpenZFS Member", {"zfs", "zfs_member"},
            read, "exact-bounded", "zfs-native", MapAdapter::NativeMap, {},
            QualificationMode::None, {}, {"summary", "exact"}});
        validate_backend_registry(result);
        return result;
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