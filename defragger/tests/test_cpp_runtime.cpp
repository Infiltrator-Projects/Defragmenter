// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "helper_policy.hpp"
#include "json.hpp"
#include "map.hpp"
#include "process.hpp"

#include <chrono>
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
    ok = check(registry.size() == 41U, "registry size") && ok;

    // Each EXT variant is independently registered; no format impersonation.
    for (const char* expected : {"ext2", "ext3", "ext4"}) {
        const BackendInfo* ext = backend_by_fstype(expected);
        ok = check(ext != nullptr, "independent EXT backend lookup") && ok;
        if (ext != nullptr) {
            ok = check(ext->id == expected, "EXT backend keeps its own identity") && ok;
            ok = check((ext->capabilities & CAP_DEFRAG) != 0U,
                       "EXT backend advertises qualified defrag") && ok;
            ok = check(operation_for(*ext, "growth-defrag") != nullptr,
                       "EXT growth-defrag operation") && ok;
            ok = check(ext->worker == "operation-engine",
                       "EXT backend retains common ABI dispatch worker") && ok;
        }
    }

    const BackendInfo* vfat = backend_by_fstype("vfat");
    ok = check(vfat != nullptr, "vfat alias lookup") && ok;
    if (vfat != nullptr)
        ok = check(vfat->id == "fat32", "vfat maps to fat32") && ok;

    const BackendInfo* apfs = backend_by_fstype("apfs");
    ok = check(apfs != nullptr, "APFS lookup") && ok;
    if (apfs != nullptr) {
        ok = check(apfs->map_accuracy == "exact-bounded-spaceman",
                   "APFS registry matches native map accuracy") && ok;
        ok = check((apfs->capabilities & CAP_DEFRAG) != 0U,
                   "APFS advertises bounded defrag") && ok;
        ok = check(operation_for(*apfs, "growth-defrag") != nullptr,
                   "APFS growth-defrag operation") && ok;
        ok = check(operation_for(*apfs, "recover") != nullptr,
                   "APFS recover operation") && ok;
    }

    const BackendInfo* zfs = backend_by_fstype("zfs");
    ok = check(zfs != nullptr, "ZFS lookup") && ok;
    if (zfs != nullptr) {
        ok = check(zfs->map_accuracy == "exact-bounded",
                   "ZFS advertises bounded exact analysis") && ok;
        ok = check((zfs->capabilities & CAP_DEFRAG) == 0U &&
                   (zfs->capabilities & CAP_GROWTH_DEFRAG) == 0U &&
                   (zfs->capabilities & CAP_RECOVER) == 0U,
                   "ZFS mutation remains out of product scope") && ok;
        ok = check(zfs->operations.empty(),
                   "ZFS exposes no raw mutation operations") && ok;
    }

    const BackendInfo* fat12 = backend_by_fstype("fat12");
    ok = check(fat12 != nullptr, "FAT12 lookup") && ok;
    if(fat12) {
        const Json payload=Json::parse(
            "{\"filesystem\":\"fat12\",\"abi\":2,\"unit_size\":512,"
            "\"total_units\":4,\"free_units\":2,\"allocated_ranges\":[[0,2]],"
            "\"fragmented_ranges\":[[1,2]],\"regular_files\":1,\"fragmented_files\":1,"
            "\"directories\":0,\"fragmented_directories\":0,\"defrag_qualified\":false,"
            "\"growth_qualified\":false,\"recovery_qualified\":false}");
        try {
            const Json mapped=map_v2_response(*fat12,payload,2U);
            ok=check(mapped.at("unknown_bytes").unsigned_value()==0U &&
                     mapped.at("free_bytes").unsigned_value()==1024U &&
                     mapped.at("fragmented_files").unsigned_value()==1U,
                     "V2 FAT exact extent presentation") && ok;
        } catch (...) {ok=check(false,"V2 FAT map") && ok;}
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

    std::string deeply_nested;
    for (unsigned index = 0U; index < 70U; ++index) deeply_nested += '[';
    deeply_nested += '0';
    for (unsigned index = 0U; index < 70U; ++index) deeply_nested += ']';
    bool rejected_deep_json = false;
    try {
        (void)Json::parse(deeply_nested);
    } catch (const std::runtime_error&) {
        rejected_deep_json = true;
    }
    ok = check(rejected_deep_json, "JSON nesting depth is bounded") && ok;

    const CommandResult command =
        run_capture({"/bin/sh", "-c", "printf native-cpp; printf warning >&2"});
    ok = check(command.return_code == 0, "process exit code") && ok;
    ok = check(command.standard_output == "native-cpp", "process stdout") && ok;
    ok = check(command.standard_error == "warning", "process stderr") && ok;

    std::vector<std::string> observed;
    const CommandResult streamed = run_capture(
        {"/bin/sh", "-c", "printf '@@ANA' >&2; printf 'LYSIS phase\\nwarning\\n' >&2; printf map-json"},
        1024U, std::chrono::seconds(5),
        [&](const std::string& line) { observed.push_back(line); });
    ok = check(observed == std::vector<std::string>{"@@ANALYSIS phase", "warning"},
               "stderr observation assembles complete lines") && ok;
    ok = check(streamed.standard_output == "map-json" &&
               streamed.standard_error == "@@ANALYSIS phase\nwarning\n",
               "progress observation preserves separate stdout and stderr") && ok;

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

    bool rejected_timeout = false;
    try {
        (void)run_capture(
            {"/bin/sh", "-c", "sleep 2"},
            1024U, std::chrono::milliseconds(50));
    } catch (const std::runtime_error&) {
        rejected_timeout = true;
    }
    ok = check(rejected_timeout, "child execution timeout is enforced") && ok;

    const BackendInfo* btrfs=backend_by_fstype("btrfs");
    ok=check(btrfs!=nullptr,"Btrfs lookup") && ok;
    if(btrfs) {
        const auto test_map=[&](const char* json,bool accepted) {
            bool passed=false;
            try {
                const Json map=map_v2_response(*btrfs,Json::parse(json),1U);
                passed=map.at("map_accuracy").string()=="summary" &&
                    map.at("unknown_bytes").unsigned_value()==3584U &&
                    !map.at("defrag_qualified").boolean();
            } catch (...) {}
            return passed==accepted;
        };
        ok=check(test_map("{\"filesystem\":\"btrfs\",\"abi\":2,\"device_bytes\":4096,"
            "\"file_extents\":[[0,512,0]],\"regular_files\":1,\"fragmented_files\":0}",true),
            "Common V2 catalogue preserves unknown allocation") && ok;
        const auto qualified = map_v2_response(*btrfs,Json::parse(
            R"({"filesystem":"btrfs","abi":2,"device_bytes":4096,"file_extents":[[0,512,0]],"regular_files":1,"fragmented_files":0,"provider_plan_preflight":true,"defrag_qualified":true,"growth_qualified":false})"),1U);
        ok=check(qualified.at("unknown_bytes").unsigned_value()==3584U &&
            qualified.at("map_accuracy").string()=="summary" &&
            qualified.at("defrag_qualified").boolean() &&
            !qualified.at("growth_qualified").boolean(),
            "Read-only provider plan qualifies only its own policy without inventing allocation") && ok;
        const auto complete=map_v2_response(*btrfs,Json::parse(
            R"({"filesystem":"btrfs","abi":2,"device_bytes":4096,"file_extents":[[0,512,0]],"regular_files":1,"fragmented_files":0,"allocation_ranges":[[0,512,2],[512,1024,3],[1024,4096,1]],"allocation_unknown_bytes":0,"allocation_free_bytes":3072})"),1U);
        ok=check(complete.at("map_accuracy").string()=="exact-allocation" &&
            complete.at("unknown_bytes").unsigned_value()==0U &&
            complete.at("reserved_bytes").unsigned_value()==512U &&
            complete.at("allocated_bytes").unsigned_value()==512U &&
            complete.at("free_bytes").unsigned_value()==3072U,
            "Canonical whole-volume allocation retains reserved and free space") && ok;
        const auto partial=map_v2_response(*btrfs,Json::parse(
            R"({"filesystem":"btrfs","abi":2,"device_bytes":4096,"file_extents":[[0,512,0]],"regular_files":1,"fragmented_files":0,"allocation_ranges":[[0,512,2],[512,4096,0]],"allocation_unknown_bytes":3584,"allocation_free_bytes":0})"),1U);
        ok=check(partial.at("map_accuracy").string()=="summary" &&
            partial.at("unknown_bytes").unsigned_value()==3584U,
            "Explicit unknown allocation remains unknown") && ok;
        bool contradicted=false;
        try {
            (void)map_v2_response(*btrfs,Json::parse(
                R"({"filesystem":"btrfs","abi":2,"device_bytes":4096,"file_extents":[[0,512,0]],"regular_files":1,"fragmented_files":0,"allocation_ranges":[[0,4096,1]],"allocation_unknown_bytes":0,"allocation_free_bytes":4096})"),1U);
        } catch(const std::exception&) { contradicted=true; }
        ok=check(contradicted,"File extent cannot overlap canonical free allocation") && ok;
        ok=check(test_map("{\"filesystem\":\"zfs\",\"abi\":2,\"device_bytes\":4096,"
            "\"file_extents\":[],\"regular_files\":0,\"fragmented_files\":0}",false),
            "Foreign provider rejected") && ok;
        ok=check(test_map("{\"filesystem\":\"btrfs\",\"abi\":1,\"device_bytes\":4096,"
            "\"file_extents\":[],\"regular_files\":0,\"fragmented_files\":0}",false),
            "Legacy response rejected") && ok;
        ok=check(test_map("{\"filesystem\":\"btrfs\",\"abi\":2,\"device_bytes\":4096,"
            "\"file_extents\":[[0,4097,0]],\"regular_files\":1,\"fragmented_files\":0}",false),
            "Out-of-range V2 extent rejected") && ok;
    }

    const HelperCommand helper = helper_command(
        "operation-engine",
        {"defrag", "/dev/test", "--filesystem", "ext4", "--write",
         "--confirm", "/dev/test"},
        1000U);
    ok = check(
        helper.executable ==
            "/System/Commands/Defragmenter/defragmenter-operation-engine",
        "privileged operation-engine allowlist") && ok;
    bool rejected_journal = false;
    try {
        (void)helper_command(
            "operation-engine",
            {"defrag", "/dev/test", "--filesystem", "ext4", "--write",
             "--confirm", "/dev/test", "--journal",
             "/tmp/not-allowed.journal"},
            1000U);
    } catch (const std::exception&) {
        rejected_journal = true;
    }
    ok = check(rejected_journal, "privileged journal boundary") && ok;

    bool rejected_diagnostic_log = false;
    try {
        (void)helper_command(
            "operation-engine",
            {"defrag", "/dev/test", "--filesystem", "fat32", "--write",
             "--confirm", "/dev/test",
             "--diagnostic-log", "/etc/defragger-root.log"},
            1000U);
    } catch (const std::exception&) {
        rejected_diagnostic_log = true;
    }
    ok = check(rejected_diagnostic_log,
               "privileged worker rejects arbitrary path options") && ok;

    bool rejected_confirm = false;
    try {
        (void)helper_command(
            "operation-engine",
            {"defrag", "/dev/test", "--filesystem", "fat32", "--write",
             "--confirm", "/dev/other"},
            1000U);
    } catch (const std::exception&) {
        rejected_confirm = true;
    }
    ok = check(rejected_confirm,
               "privileged confirmation must match target") && ok;

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
