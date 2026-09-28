// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_policy.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace defragger {
namespace {
std::string value(const Json& obj, std::string_view key) {
    const auto* field = obj.find(key);
    return field ? field->string_or() : std::string{};
}
bool flag(const Json& obj, std::string_view key) {
    const auto* field = obj.find(key);
    if (!field) return false;
    if (field->is_bool()) return field->boolean();
    const auto text = field->string_or();
    return field->unsigned_or() != 0 || text == "true" || text == "1" || text == "yes";
}
void visit(const Json& node, std::vector<DesktopVolume>& result) {
    if (!node.is_object()) return;
    std::string filesystem = value(node, "fstype");
    std::transform(filesystem.begin(), filesystem.end(), filesystem.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const std::string label = value(node, "partlabel");
    if (filesystem.empty()) {
        if (label == "LD_OFS") filesystem = "ofs";
        if (label == "LD_FFS") filesystem = "ffs";
        if (label == "LD_SFS") filesystem = "sfs";
        if (label == "LD_PFS3") filesystem = "pfs3";
        if (label == "LD_APFS") filesystem = "apfs";
    }
    const bool generic_fat = filesystem == "fat" || filesystem == "vfat" || filesystem == "msdos";
    if ((backend_by_fstype(filesystem) || generic_fat) && !value(node, "path").empty()) {
        DesktopVolume volume;
        volume.path = value(node, "path");
        volume.filesystem = filesystem;
        volume.label = value(node, "label");
        if (volume.label.empty()) volume.label = label;
        volume.filesystem_uuid = value(node, "uuid");
        volume.partition_uuid = value(node, "partuuid");
        if (const auto* size = node.find("size")) volume.size = size->unsigned_or();
        volume.readonly = flag(node, "ro");
        if (const auto* mounts = node.find("mountpoints"); mounts && mounts->is_array()) {
            for (const auto& mount : mounts->array())
                volume.mounted |= !mount.string_or().empty();
        }
        // lsblk routing hints do not establish the identity required for raw writes.
        result.push_back(std::move(volume));
    }
    if (const auto* children = node.find("children"); children && children->is_array())
        for (const auto& child : children->array()) visit(child, result);
}
} // namespace

std::vector<DesktopVolume> desktop_discover(const Json& lsblk) {
    std::vector<DesktopVolume> volumes;
    if (const auto* nodes = lsblk.find("blockdevices"); nodes && nodes->is_array())
        for (const auto& node : nodes->array()) visit(node, volumes);
    return volumes;
}

DesktopControls desktop_controls(const DesktopVolume* v, bool busy,
                                 bool stopping, bool journal_exists) {
    DesktopControls out;
    out.stop = busy && !stopping;
    if (!v || busy) return out;
    out.analyse = true;
    out.unmount = v->mounted && !v->image;
    const auto* backend = backend_by_fstype(v->filesystem);
    if (!backend || !v->verified || v->readonly || v->mounted) return out;
    out.defrag = v->exact_analysis && !journal_exists &&
                 operation_for(*backend, "defrag");
    out.growth_defrag = v->exact_analysis && !journal_exists &&
                        operation_for(*backend, "growth-defrag");
    // Recovery must remain available after restart even before a fresh map is
    // produced; the journal and worker rebind the exact target identity.
    out.recover = journal_exists && operation_for(*backend, "recover");
    return out;
}

std::string desktop_journal(const DesktopVolume& v, unsigned uid) {
    std::string identity = v.filesystem_uuid;
    if (!v.partition_uuid.empty()) {
        if (!identity.empty()) identity += '|';
        identity += v.partition_uuid;
    }
    const bool have_stable_identity = !identity.empty();
    const std::string material =
        have_stable_identity ? identity : v.path;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest);
    std::string stem = have_stable_identity ? "volume" : v.path;
    while (!stem.empty() && stem.front() == '/') stem.erase(stem.begin());
    for (auto& ch : stem)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '.' && ch != '-') ch = '_';
    if (stem.size() > 80) stem.erase(0, stem.size() - 80);
    if (stem.empty()) stem = "volume";
    char suffix[17];
    for (unsigned i = 0; i < 8; ++i) std::snprintf(suffix + i * 2, 3, "%02x", digest[i]);
    return "/var/lib/linux-defragger/state/" + std::to_string(uid) + "/" + stem + "-" + suffix + ".journal";
}

void desktop_verify_identity(DesktopVolume& v, std::string_view detected) {
    const auto* candidate = backend_by_fstype(v.filesystem);
    const auto* actual = backend_by_fstype(detected);
    const bool generic_fat = v.filesystem == "fat" || v.filesystem == "vfat" || v.filesystem == "msdos";
    const bool fat_variant = detected == "fat12" || detected == "fat16" || detected == "fat32";
    if (!actual || (!generic_fat && candidate != actual) || (generic_fat && !fat_variant))
        throw std::runtime_error("Native filesystem identity conflicts with discovery metadata");
    v.filesystem = std::string(detected);
    // Identity verification alone never authorises a raw write.  A fresh
    // exact allocation analysis must establish that separately.
    v.exact_analysis = false;
    v.verified = true;
}

std::vector<std::string> desktop_mutation(const DesktopVolume& v,
                                          std::string_view operation,
                                          std::string_view engine,
                                          std::string_view journal,
                                          unsigned cells,
                                          bool journal_exists) {
    const auto* backend = backend_by_fstype(v.filesystem);
    if (!backend || !operation_for(*backend, operation) || !v.verified ||
        v.mounted || v.readonly)
        throw std::runtime_error("Volume is not verified, unmounted, writable or supported");
    if (operation != "recover" && !v.exact_analysis)
        throw std::runtime_error(
            "An exact allocation analysis is required before filesystem mutation");
    if ((operation == "recover") != journal_exists)
        throw std::runtime_error("An unfinished journal must be recovered before further writes");
    if (journal.empty()) throw std::runtime_error("A persistent journal path is required");
    std::vector<std::string> args = {std::string(engine), std::string(operation), v.path,
        "--filesystem", v.filesystem, "--write", "--confirm", v.path, "--journal", std::string(journal)};
    if (operation == "growth-defrag") { args.emplace_back("--growth-percent"); args.emplace_back("10"); }
    if (operation != "recover") {
        args.insert(args.end(), {"--batch-clusters", "4096"});
    }
    // Worker concurrency is engine-owned. FAT and NTFS already select their
    // useful N-1 CPU budget internally; the other engines must not receive a
    // cosmetic --workers option that they merely ignore.
    args.insert(args.end(), {"--ram-buffer", "auto"});
    if (operation != "recover") {
        args.emplace_back("--live-map-cells");
        args.emplace_back(std::to_string(std::clamp(cells, 256U, 1048576U)));
    }
    return args;
}
} // namespace defragger
