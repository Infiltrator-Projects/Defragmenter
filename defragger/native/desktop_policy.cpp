// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_policy.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <utility>

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
std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return text;
}
std::string filesystem_from_test_media_label(std::string_view label) {
    constexpr std::string_view prefix = "LD_";
    if (label.size() <= prefix.size() || label.substr(0, prefix.size()) != prefix)
        return {};

    std::string candidate = lower_ascii(std::string(label.substr(prefix.size())));
    const auto* backend = backend_by_fstype(candidate);
    return backend == nullptr ? std::string{} : candidate;
}
void visit(const Json& node, std::vector<DesktopVolume>& result) {
    if (!node.is_object()) return;
    std::string filesystem = lower_ascii(value(node, "fstype"));

    const std::string filesystem_label = value(node, "label");
    const std::string partition_label = value(node, "partlabel");
    const std::string discovery_label =
        !partition_label.empty() ? partition_label : filesystem_label;

    if (filesystem.empty()) {
        /*
         * Test Media intentionally includes formats that lsblk/blkid may not
         * identify on every host. Its LD_<registry-id-or-alias> GPT labels are
         * discovery hints only; the native analyser still has to prove the real
         * filesystem identity before any write is enabled. Keep the label
         * convention tied to the registry instead of maintaining a second
         * filesystem catalogue here.
         */
        filesystem = filesystem_from_test_media_label(discovery_label);
    }
    const bool generic_fat = filesystem == "fat" || filesystem == "vfat" || filesystem == "msdos";
    if ((backend_by_fstype(filesystem) || generic_fat) && !value(node, "path").empty()) {
        DesktopVolume volume;
        volume.path = value(node, "path");
        volume.filesystem = filesystem;
        volume.label = filesystem_label;
        if (volume.label.empty()) volume.label = partition_label;
        volume.filesystem_uuid = value(node, "uuid");
        volume.partition_uuid = value(node, "partuuid");
        volume.serial = value(node, "serial");
        volume.wwn = value(node, "wwn");
        if (const auto* size = node.find("size")) volume.size = size->unsigned_or();
        if (const auto* start = node.find("start"))
            volume.start_sector = start->unsigned_or();
        volume.readonly = flag(node, "ro");
        if (const auto* mounts = node.find("mountpoints"); mounts != nullptr) {
            if (mounts->is_array()) {
                for (const auto& mount : mounts->array())
                    volume.mounted |= !mount.string_or().empty();
            } else {
                volume.mounted = !mounts->string_or().empty();
            }
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
    if (!backend || v->readonly || v->mounted) return out;

    // Recovery admission comes from the persistent transaction, not a source
    // scan that may be impossible halfway through committing that transaction.
    // The native worker validates journal schema, target binding and staged
    // bytes before writing; discovery metadata only selects that validator.
    out.recover = journal_exists && operation_for(*backend, "recover");
    if (!v->verified) return out;

    /*
     * Once native identity has been proved and the target is writable and
     * unmounted, operation controls are entry points rather than the mutation
     * authority.  Keep supported Defragment/Growth Defrag controls clickable
     * even when exact writer qualification rejected this layout or a journal
     * needs recovery, so desktop_mutation() can report the precise reason
     * instead of presenting an unexplained dead button.  The destructive path
     * still re-checks every prerequisite before constructing an engine command.
     */
    out.defrag = operation_for(*backend, "defrag") != nullptr;
    out.growth_defrag = operation_for(*backend, "growth-defrag") != nullptr;
    return out;
}

std::string desktop_journal(const DesktopVolume& v, unsigned uid) {
    std::string identity = v.filesystem_uuid;
    if (!v.partition_uuid.empty()) {
        if (!identity.empty()) identity += '|';
        identity += v.partition_uuid;
    }
    if (identity.empty() && (!v.serial.empty() || !v.wwn.empty())) {
        identity = "media:";
        identity += v.serial;
        identity += '|';
        identity += v.wwn;
        identity += '|';
        identity += std::to_string(v.start_sector);
        identity += '|';
        identity += std::to_string(v.size);
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
    return "/var/lib/defragmenter/state/" + std::to_string(uid) + "/" + stem + "-" + suffix + ".journal";
}

void desktop_verify_identity(DesktopVolume& v, std::string_view detected) {
    const auto* candidate = backend_by_fstype(v.filesystem);
    const auto* actual = backend_by_fstype(detected);
    const bool generic_fat =
        v.filesystem == "fat" || v.filesystem == "vfat" ||
        v.filesystem == "msdos";
    const bool fat_variant =
        actual != nullptr &&
        (actual->id == "fat12" || actual->id == "fat16" ||
         actual->id == "fat32");
    if (!actual || (!generic_fat && candidate != actual) ||
        (generic_fat && !fat_variant))
        throw std::runtime_error(
            "Native filesystem identity conflicts with discovery metadata");

    /*
     * Store the registry's canonical lower-case identity rather than the
     * worker's display spelling (for example FAT16). Discovery aliases such
     * as vfat are allowed to resolve to the exact FAT width proven by the
     * native analyser.
     */
    v.filesystem = actual->id;
    // Identity verification alone never authorises a raw write. A fresh
    // exact allocation analysis must establish that separately.
    v.exact_analysis = false;
    v.defrag_qualified = false;
    v.growth_qualified = false;
    v.defrag_reason.clear();
    v.growth_reason.clear();
    v.verified = true;
}

std::vector<std::string> desktop_mutation(const DesktopVolume& v,
                                          std::string_view operation,
                                          std::string_view engine,
                                          std::string_view journal,
                                          unsigned cells,
                                          bool journal_exists) {
    const auto* backend = backend_by_fstype(v.filesystem);
    const auto* spec = backend ? operation_for(*backend, operation) : nullptr;
    if (!backend || !spec || (operation != "recover" && !v.verified) ||
        v.mounted || v.readonly)
        throw std::runtime_error("Volume is not verified, unmounted, writable or supported");
    if (operation != "recover" && !v.exact_analysis)
        throw std::runtime_error(
            "An exact allocation analysis is required before filesystem mutation");
    if (operation == "defrag" && !v.defrag_qualified)
        throw std::runtime_error(
            v.defrag_reason.empty()
                ? "This exact filesystem layout is outside the qualified Defragment writer contract"
                : v.defrag_reason);
    if (operation == "growth-defrag" && !v.growth_qualified)
        throw std::runtime_error(
            v.growth_reason.empty()
                ? "This exact filesystem layout is outside the qualified Growth Defrag writer contract"
                : v.growth_reason);
    if ((operation == "recover") != journal_exists)
        throw std::runtime_error("An unfinished journal must be recovered before further writes");
    if (journal.empty()) throw std::runtime_error("A persistent journal path is required");
    std::vector<std::string> args = {std::string(engine), std::string(operation), v.path,
        "--filesystem", v.filesystem, "--write", "--confirm", v.path, "--journal", std::string(journal)};
    args.insert(args.end(), spec->default_options.begin(), spec->default_options.end());
    if (operation == "growth-defrag") {
        args.emplace_back("--growth-percent");
        args.emplace_back("10");
    }
    if (operation != "recover") {
        args.emplace_back("--live-map-cells");
        args.emplace_back(std::to_string(std::clamp(cells, 256U, 1048576U)));
    }
    return args;
}
} // namespace defragger
