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
    return {std::move(name), std::move(worker), std::move(policy),
            std::move(label), std::move(description), std::move(warning), {},
            true, true};
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
    {"hfsplus-native", "LINUX_DEFRAGGER_HFSPLUS_WORKER",
     "/usr/lib/linux-defragger/filesystems/hfsplus/linux-defragger-hfsplus-worker",
     "linux-defragger-hfsplus-worker"},
    {"hfs-native", "LINUX_DEFRAGGER_HFS_ANALYSER",
     "/usr/lib/linux-defragger/filesystems/hfs/hfs_analyser", "hfs_analyser"},
    {"affs-native", "LINUX_DEFRAGGER_AFFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/affs/linux-defragger-affs-worker",
     "linux-defragger-affs-worker"},
    {"btrfs-native", "LINUX_DEFRAGGER_BTRFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/btrfs/linux-defragger-btrfs-worker",
     "linux-defragger-btrfs-worker"},
    {"fat-native", "LINUX_DEFRAGGER_FAT_WORKER",
     "/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker",
     "linux-defragger-fat-worker"},
    {"exfat-native", "LINUX_DEFRAGGER_EXFAT_WORKER",
     "/usr/lib/linux-defragger/filesystems/exfat/linux-defragger-exfat-worker",
     "linux-defragger-exfat-worker"},
    {"ntfs-native", "LINUX_DEFRAGGER_NTFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/ntfs/linux-defragger-ntfs-worker",
     "linux-defragger-ntfs-worker"},
    {"ext-native", "LINUX_DEFRAGGER_EXT_WORKER",
     "/usr/lib/linux-defragger/filesystems/ext4/linux-defragger-ext-worker",
     "linux-defragger-ext-worker"},
    {"ext-metadata", "LINUX_DEFRAGGER_EXT_METADATA_WORKER",
     "/usr/lib/linux-defragger/filesystems/ext4/linux-defragger-ext-metadata-worker",
     "linux-defragger-ext-metadata-worker"},
    {"xfs-native", "LINUX_DEFRAGGER_XFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/xfs/linux-defragger-xfs-worker",
     "linux-defragger-xfs-worker"},
    {"apfs-native", "LINUX_DEFRAGGER_APFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/apfs/linux-defragger-apfs-worker",
     "linux-defragger-apfs-worker"},
    {"minix-native", "LINUX_DEFRAGGER_MINIX_WORKER",
     "/usr/lib/linux-defragger/filesystems/minix/linux-defragger-minix-worker",
     "linux-defragger-minix-worker"},
    {"sfs-native", "LINUX_DEFRAGGER_SFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/sfs/linux-defragger-sfs-worker",
     "linux-defragger-sfs-worker"},
    {"pfs3-native", "LINUX_DEFRAGGER_PFS3_WORKER",
     "/usr/lib/linux-defragger/filesystems/pfs3/linux-defragger-pfs3-worker",
     "linux-defragger-pfs3-worker"},
    {"swap-native", "LINUX_DEFRAGGER_SWAP_WORKER",
     "/usr/lib/linux-defragger/filesystems/swap/linux-defragger-swap-worker",
     "linux-defragger-swap-worker"},
    {"ufs-native", "LINUX_DEFRAGGER_UFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/ufs/linux-defragger-ufs-worker",
     "linux-defragger-ufs-worker"},
    {"zfs-native", "LINUX_DEFRAGGER_ZFS_WORKER",
     "/usr/lib/linux-defragger/filesystems/zfs/linux-defragger-zfs-worker",
     "linux-defragger-zfs-worker"},
    {"mapper", "LINUX_DEFRAGGER_MAPPER",
     "/usr/lib/linux-defragger/linux-defragger-mapper", "linux-defragger-mapper"},
    {"operation-engine", "LINUX_DEFRAGGER_OPERATION_ENGINE",
     "/usr/lib/linux-defragger/linux-defragger-operation-engine",
     "linux-defragger-operation-engine"},
    {"helper", "LINUX_DEFRAGGER_HELPER",
     "/usr/lib/linux-defragger/linux-defragger-privileged-helper",
     "linux-defragger-privileged-helper"},
    {"test-media", "LINUX_DEFRAGGER_TEST_MEDIA",
     "/usr/bin/linux-defragger-test-media",
     "linux-defragger-test-media"},
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
                "Amiga OFS/FFS writing uses Defragmenter's offline first-party native C raw engine.",
                "Amiga Growth Defrag uses the native C raw engine and leaves an exact 10% free-block reserve after each regular file.")});
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
                "exFAT writing uses Defragmenter's offline native C raw engine. No exFAT filesystem driver, mount, filesystem ioctl or external filesystem utility is used.",
                "exFAT Growth Defrag uses the same native C raw engine and leaves an exact 10% free-cluster reserve after every regular file.")});
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
                "Classic HFS writing uses Defragmenter's offline first-party native C engine and relocates complete regular-file forks across both inline and Extents Overflow records while preserving validated B-tree topology.",
                "Classic HFS Growth Defrag leaves a 10% free allocation-block reserve after each supported non-empty file fork."),
            QualificationMode::WorkerPreflight});
        result.push_back({
            "hfsplus", "Apple HFS+/HFSX", {"hfsplus", "hfs+", "hfsx"},
            write, "exact", "hfsplus-native", MapAdapter::HfsPlus,
            standard_write_ops(
                "hfsplus-native",
                "HFS+/HFSX writing uses Defragmenter's offline first-party native C raw engine.",
                "HFS+/HFSX Growth Defrag uses the native C raw engine and leaves an exact 10% free allocation-block reserve after each movable fork."),
            QualificationMode::WorkerPreflight});
        result.push_back({
            "minix", "Minix Filesystem", {"minix", "minix2", "minix3"},
            write, "exact", "minix-native", MapAdapter::NativeMap,
            standard_write_ops(
                "minix-native",
                "Minix writing uses Defragmenter's offline first-party native C raw engine.",
                "Minix Growth Defrag leaves an exact 10% free-zone reserve after every regular file.")});
        result.push_back({
            "ntfs", "NTFS", {"ntfs", "ntfs3"}, write, "exact",
            "ntfs-native", MapAdapter::Ntfs,
            standard_write_ops(
                "ntfs-native",
                "NTFS writing uses Defragmenter's offline native C raw engine. No NTFS filesystem driver, mount, filesystem ioctl or external filesystem utility is used.",
                "NTFS Growth Defrag uses the same native C raw engine and leaves an exact 10% free-cluster reserve after every supported regular-file stream."),
            QualificationMode::MapOrWorkerPreflight});
        result.push_back({
            "pfs3", "Amiga PFS3", {"pfs3", "pfs", "professionalfilesystem"},
            write, "exact-allocation", "pfs3-native", MapAdapter::NativeMap,
            standard_write_ops(
                "pfs3-native",
                "Amiga PFS3 writing uses Defragmenter's offline first-party native C raw engine, recursively validates nested user directories, preserves validated hard links, relocates soft-link payloads and rollover files, and fails closed on unsupported SUPERINDEX/LARGEFILE or remaining special-entry modes.",
                "PFS3 Growth Defrag leaves an exact 10% free-block reserve after every supported regular file.")});
        result.push_back({
            "sfs", "Amiga SFS", {"sfs", "sfs0", "sfs2", "smartfilesystem"},
            write, "exact-allocation", "sfs-native", MapAdapter::NativeMap,
            standard_write_ops(
                "sfs-native",
                "Amiga SFS0/SFS2 writing uses Defragmenter's offline first-party native C raw engine.",
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
                "UFS1/UFS2 writing uses Defragmenter's offline first-party native C engine and is fail-closed to clean, non-journalled, snapshot-free filesystems whose regular-file allocation is fully understood.",
                "UFS Growth Defrag uses the same staged native engine and leaves an exact 10% free-fragment reserve after every supported regular file."),
            QualificationMode::WorkerPreflight, {},
            {"summary", "exact-allocation", "exact"}});
        result.push_back({
            "xfs", "XFS", {"xfs"}, write, "exact",
            "xfs-native", MapAdapter::Xfs,
            standard_write_ops(
                "xfs-native",
                "XFS writing uses Defragmenter's offline native C raw engine. No XFS kernel driver, mount, filesystem ioctl or external repair tool is used.",
                "XFS Growth Defrag uses the native C raw engine, requires staging space, and preserves an exact 10% free run after every supported regular file."),
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
    // by LINUX_DEFRAGGER_* executable environment variables.
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