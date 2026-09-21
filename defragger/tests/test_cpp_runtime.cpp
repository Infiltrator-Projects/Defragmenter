// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "helper_policy.hpp"
#include "json.hpp"
#include "map.hpp"
#include "process.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "cpp runtime test failed: %s\n", message);
    return false;
}

std::string fake_map_worker(const char* payload) {
    char path[] = "/tmp/defragger-map-worker.XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    FILE* stream = fdopen(fd, "w");
    if (stream == nullptr) {
        (void)close(fd);
        (void)unlink(path);
        throw std::runtime_error("fdopen failed");
    }
    const int written = std::fprintf(
        stream, "#!/bin/sh\nprintf '%%s\\n' '%s'\n", payload);
    const int closed = std::fclose(stream);
    if (written < 0 || closed != 0 || chmod(path, 0700) != 0) {
        (void)unlink(path);
        throw std::runtime_error("cannot create fake map worker");
    }
    return path;
}

} // namespace

int main() {
    using namespace defragger;
    bool ok = true;

    for (const std::size_t invalid_cells : {std::size_t{0U}, kMaxMapCells + 1U}) {
        bool rejected = false;
        try { (void)map_capture_limit(invalid_cells); }
        catch (const std::invalid_argument&) { rejected = true; }
        ok = check(rejected, "map capture rejects invalid cell count") && ok;
    }
    ok = check(map_capture_limit(800000U) > 64U * 1024U * 1024U,
               "large GUI maps have bounded capture space") && ok;

    const auto merged = merge_ranges(std::vector<UnitRange>{
        {5U, 7U}, {0U, 2U}, {2U, 5U}, {10U, 12U}});
    ok = check(merged.size() == 2U && merged[0].start == 0U &&
                   merged[0].end == 7U && merged[1].start == 10U &&
                   merged[1].end == 12U,
               "native range merging") && ok;
    const auto complement = complement_ranges(15U, merged);
    ok = check(complement.size() == 2U && complement[0].start == 7U &&
                   complement[0].end == 10U && complement[1].start == 12U &&
                   complement[1].end == 15U,
               "native range complement") && ok;
    Json aggregated = aggregate_ranges(
        20U, 4U, 1U, "test",
        std::vector<StateRange>{{0U, 20U, 1U}}, "exact");
    ok = check(aggregated.at("cells").array().size() == 4U,
               "native range aggregation") && ok;
    auto& aggregate_cells = aggregated.at("cells").array();
    ok = check(overlay_ranges(
                   aggregate_cells,
                   std::vector<UnitRange>{{2U, 8U}, {7U, 13U}},
                   "fragmented") == 11U,
               "native range overlay coalesces overlaps") && ok;
    bool rejected_overlap = false;
    try {
        (void)aggregate_ranges(
            20U, 4U, 1U, "test",
            std::vector<StateRange>{{0U, 10U, 1U}, {9U, 12U, 0U}},
            "exact");
    } catch (const std::runtime_error&) {
        rejected_overlap = true;
    }
    ok = check(rejected_overlap, "native aggregation rejects overlapping states") && ok;

    const auto& registry = backend_registry();
    ok = check(registry.size() == 18U, "registry size") && ok;

    const BackendInfo* ext = backend_by_fstype("EXT2");
    ok = check(ext != nullptr, "EXT2 alias lookup") && ok;
    if (ext != nullptr) {
        ok = check(ext->id == "ext4", "EXT2 maps to ext4") && ok;
        ok = check((ext->capabilities & CAP_DEFRAG) != 0U,
                   "ext4 advertises defrag") && ok;
        ok = check(operation_for(*ext, "growth-defrag") != nullptr,
                   "ext4 growth-defrag operation") && ok;
    }

    const BackendInfo* vfat = backend_by_fstype("vfat");
    ok = check(vfat != nullptr, "vfat alias lookup") && ok;
    if (vfat != nullptr)
        ok = check(vfat->id == "fat32", "vfat maps to fat32") && ok;

    const BackendInfo* apfs = backend_by_fstype("apfs");
    ok = check(apfs != nullptr, "APFS lookup") && ok;
    if (apfs != nullptr) {
        ok = check((apfs->capabilities & CAP_DEFRAG) != 0U,
                   "APFS advertises bounded defrag") && ok;
        ok = check(operation_for(*apfs, "growth-defrag") != nullptr,
                   "APFS growth-defrag operation") && ok;
        ok = check(operation_for(*apfs, "recover") != nullptr,
                   "APFS recover operation") && ok;
    }

    const BackendInfo* fat12 = backend_by_fstype("fat12");
    ok = check(fat12 != nullptr, "FAT12 lookup") && ok;
    if (fat12 != nullptr) {
        const std::string fat_worker = fake_map_worker(
            "{\"filesystem\":\"FAT12\",\"cluster_size\":512,"
            "\"data_clusters\":4,\"free_clusters\":2,\"used_clusters\":2,"
            "\"regular_files\":1,\"fragmented_files\":1,\"directories\":1,"
            "\"fragmented_directories\":0,\"free_gaps_below_highest\":1,"
            "\"cell_count\":2,\"cells\":["
            "{\"start\":2,\"end\":3,\"free\":0,\"used\":2,"
            "\"fragmented\":1,\"directory\":1,\"bad\":0},"
            "{\"start\":4,\"end\":5,\"free\":2,\"used\":0,"
            "\"fragmented\":0,\"directory\":0,\"bad\":0}]}");
        (void)setenv("LINUX_DEFRAGGER_FAT_WORKER", fat_worker.c_str(), 1);
        try {
            const Json mapped = map_backend(*fat12, "/dev/null", 2U);
            ok = check(mapped.at("filesystem").string() == "FAT12",
                       "FAT-specific map contract accepted") && ok;
            ok = check(mapped.at("data_clusters").unsigned_value() == 4U,
                       "FAT map geometry preserved") && ok;
        } catch (...) {
            ok = check(false, "FAT-specific map contract accepted") && ok;
        }
        (void)unlink(fat_worker.c_str());
        (void)unsetenv("LINUX_DEFRAGGER_FAT_WORKER");
    }


    const BackendInfo* ufs = backend_by_fstype("ufs2");
    ok = check(ufs != nullptr, "UFS2 alias lookup") && ok;
    if (ufs != nullptr) {
        const std::string exact_worker = fake_map_worker(
            "{\"schema\":1,\"backend\":\"read-only-domain\","
            "\"filesystem\":\"ufs\",\"map_accuracy\":\"exact\","
            "\"unit_size\":4096,\"total_units\":1,\"cell_count\":1,"
            "\"cells\":[{\"start\":0,\"end\":0,\"free\":0,\"used\":1,"
            "\"unknown\":0,\"bad\":0,\"fragmented\":0,\"directory\":0}]}");
        (void)setenv("LINUX_DEFRAGGER_UFS_WORKER", exact_worker.c_str(), 1);
        try {
            const Json mapped = map_backend(*ufs, "/dev/null", 1U);
            ok = check(mapped.at("map_accuracy").string() == "exact",
                       "UFS exact native map contract accepted") && ok;
        } catch (...) {
            ok = check(false, "UFS exact native map contract accepted") && ok;
        }
        (void)unlink(exact_worker.c_str());
        (void)unsetenv("LINUX_DEFRAGGER_UFS_WORKER");
    }

    const std::vector<std::string> input{
        "--keep", "a", "--drop", "value", "--drop=other", "--tail"};
    const std::vector<std::string> blocked{"--drop"};
    const auto output = without_options(input, blocked);
    const std::vector<std::string> expected{"--keep", "a", "--tail"};
    ok = check(output == expected, "unsupported option filtering") && ok;

    const Json parsed = Json::parse(
        R"({"text":"A\nB","number":1234567890123,"truth":true,"array":[1,null]})");
    ok = check(parsed.at("text").string() == "A\nB", "JSON string escape") && ok;
    ok = check(parsed.at("number").unsigned_value() == 1234567890123ULL,
               "JSON exact integer") && ok;
    ok = check(Json::parse(parsed.dump()).at("truth").boolean(),
               "JSON round trip") && ok;

    const CommandResult command =
        run_capture({"/bin/sh", "-c", "printf native-cpp; printf warning >&2"});
    ok = check(command.return_code == 0, "process exit code") && ok;
    ok = check(command.standard_output == "native-cpp", "process stdout") && ok;
    ok = check(command.standard_error == "warning", "process stderr") && ok;

    const CommandResult exact_limit =
        run_capture({"/bin/sh", "-c", "printf 1234"}, 4U);
    ok = check(exact_limit.standard_output == "1234",
               "exact output limit is accepted") && ok;
    bool rejected_over_limit = false;
    try {
        (void)run_capture({"/bin/sh", "-c", "printf 12345"}, 4U);
    } catch (const std::runtime_error&) {
        rejected_over_limit = true;
    }
    ok = check(rejected_over_limit, "output beyond limit is rejected") && ok;

    const BackendInfo* btrfs = backend_by_fstype("btrfs");
    ok = check(btrfs != nullptr, "Btrfs lookup") && ok;
    if (btrfs != nullptr) {
        const std::string valid_worker = fake_map_worker(
            "{\"schema\":1,\"backend\":\"read-only-domain\","
            "\"filesystem\":\"btrfs\",\"map_accuracy\":\"exact-single-device\","
            "\"unit_size\":4096,\"total_units\":1,\"cell_count\":1,"
            "\"cells\":[{\"start\":0,\"end\":0}]}");
        (void)setenv("LINUX_DEFRAGGER_BTRFS_WORKER", valid_worker.c_str(), 1);
        try {
            const Json mapped = map_backend(*btrfs, "/dev/null", 1U);
            ok = check(mapped.at("filesystem").string() == "btrfs",
                       "valid native map accepted") && ok;
        } catch (...) {
            ok = check(false, "valid native map accepted") && ok;
        }
        (void)unlink(valid_worker.c_str());

        const std::string wrong_identity = fake_map_worker(
            "{\"schema\":1,\"backend\":\"read-only-domain\","
            "\"filesystem\":\"zfs\",\"map_accuracy\":\"exact-single-device\","
            "\"unit_size\":4096,\"total_units\":1,\"cell_count\":1,"
            "\"cells\":[{\"start\":0,\"end\":0}]}");
        (void)setenv("LINUX_DEFRAGGER_BTRFS_WORKER", wrong_identity.c_str(), 1);
        bool rejected_identity = false;
        try {
            (void)map_backend(*btrfs, "/dev/null", 1U);
        } catch (const std::runtime_error&) {
            rejected_identity = true;
        }
        ok = check(rejected_identity, "native map identity mismatch rejected") && ok;
        (void)unlink(wrong_identity.c_str());

        const std::string wrong_accuracy = fake_map_worker(
            "{\"schema\":1,\"backend\":\"read-only-domain\","
            "\"filesystem\":\"btrfs\",\"map_accuracy\":\"summary\","
            "\"unit_size\":4096,\"total_units\":1,\"cell_count\":1,"
            "\"cells\":[{\"start\":0,\"end\":0}]}");
        (void)setenv("LINUX_DEFRAGGER_BTRFS_WORKER", wrong_accuracy.c_str(), 1);
        bool rejected_accuracy = false;
        try {
            (void)map_backend(*btrfs, "/dev/null", 1U);
        } catch (const std::runtime_error&) {
            rejected_accuracy = true;
        }
        ok = check(rejected_accuracy, "native map accuracy mismatch rejected") && ok;
        (void)unlink(wrong_accuracy.c_str());
        (void)unsetenv("LINUX_DEFRAGGER_BTRFS_WORKER");
    }

    const HelperCommand helper = helper_command(
        "operation-engine",
        {"defrag", "/dev/test", "--filesystem", "ext4",
         "--journal", "/var/lib/linux-defragger/state/1000/test.journal"},
        1000U);
    ok = check(
        helper.executable ==
            "/usr/lib/linux-defragger/linux-defragger-operation-engine",
        "privileged operation-engine allowlist") && ok;
    bool rejected_journal = false;
    try {
        (void)helper_command(
            "operation-engine",
            {"defrag", "/dev/test", "--filesystem", "ext4",
             "--journal", "/tmp/not-allowed.journal"},
            1000U);
    } catch (const std::exception&) {
        rejected_journal = true;
    }
    ok = check(rejected_journal, "privileged journal boundary") && ok;

    const std::string manifest = registry_manifest_json();
    ok = check(manifest.find("\"schema\":3") != std::string::npos,
               "manifest schema") && ok;
    ok = check(manifest.find("\"id\":\"ntfs\"") != std::string::npos,
               "manifest NTFS entry") && ok;
    ok = check(manifest.find("\"id\":\"hfsplus\"") != std::string::npos,
               "manifest HFS+ entry") && ok;
    ok = check(manifest.find("\"id\":\"pfs3\"") != std::string::npos,
               "manifest PFS3 entry") && ok;

    return ok ? 0 : 1;
}
