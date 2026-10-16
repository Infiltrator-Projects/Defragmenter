// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_policy.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

// Checks remain executable under NDEBUG in optimized qualification builds.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        std::exit(EXIT_FAILURE); \
    } \
} while (0)

int main() {
    using namespace defragger;
    auto volumes = desktop_discover(Json::parse(R"({"blockdevices":[{"path":"/dev/sdb","children":[{"path":"/dev/sdb1","fstype":"ext4","size":1048576,"mountpoints":[null],"uuid":"UUID","partuuid":"PART"}]}]})"));
    CHECK(volumes.size() == 1);
    auto& v = volumes.front();
    CHECK(v.path == "/dev/sdb1" && !v.verified && !v.mounted);

    auto labelled = desktop_discover(Json::parse(
        R"({"blockdevices":[{"path":"/dev/mmcblk0","children":[{"path":"/dev/mmcblk0p1","fstype":null,"label":"LD_FAT12","partlabel":"LD_FAT12","size":267386880,"mountpoints":[null]}]}]})"));
    CHECK(labelled.size() == 1);
    CHECK(labelled.front().path == "/dev/mmcblk0p1");
    CHECK(labelled.front().filesystem == "fat12");
    CHECK(labelled.front().label == "LD_FAT12");
    CHECK(!labelled.front().mounted);

    auto controls = desktop_controls(&v, false, false, false);
    CHECK(controls.analyse);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);

    try {
        (void)desktop_mutation(
            v, "defrag", "/tmp/engine", "/tmp/test.journal", 512, false);
        CHECK(false);
    } catch (const std::runtime_error&) {}

    CHECK(desktop_journal(v, 1000) ==
        "/var/lib/linux-defragger/state/1000/volume-fc1f33e16c4a68d6.journal");
    try {
        desktop_verify_identity(v, "ntfs");
        CHECK(false);
    } catch (const std::runtime_error&) {}
    CHECK(!v.verified);
    desktop_verify_identity(v, "ext4");
    CHECK(v.verified);

    /*
     * Once identity is verified and the target is writable and offline,
     * supported operations stay clickable even when exact qualification has
     * not authorised a write.  desktop_mutation() remains the fail-closed
     * authority and supplies the precise rejection reason.
     */
    controls = desktop_controls(&v, false, false, false);
    CHECK(controls.defrag);
    CHECK(controls.growth_defrag);

    v.exact_analysis = true;
    v.defrag_qualified = true;
    v.growth_qualified = true;
    CHECK(desktop_controls(&v, false, false, false).defrag);
    CHECK(desktop_controls(&v, false, false, false).growth_defrag);

    DesktopVolume fat = v;
    fat.filesystem = "vfat";
    fat.verified = false;
    desktop_verify_identity(fat, "FAT16");
    CHECK(fat.verified && fat.filesystem == "fat16" &&
           !fat.exact_analysis && !fat.defrag_qualified &&
           !fat.growth_qualified);

    DesktopVolume removable = v;
    removable.filesystem_uuid.clear();
    removable.partition_uuid.clear();
    removable.serial = "DISK-SERIAL";
    removable.wwn = "0x5000000000000001";
    removable.start_sector = 2048U;
    removable.size = 1048576U;
    const auto stable_journal = desktop_journal(removable, 1000);
    removable.path = "/dev/sdc1";
    CHECK(desktop_journal(removable, 1000) == stable_journal);

    const auto journal = desktop_journal(v, 1000);
    DesktopVolume rejected = v;
    rejected.defrag_qualified = false;
    rejected.defrag_reason = "qualified writer rejected feature X";
    CHECK(desktop_controls(&rejected, false, false, false).defrag);
    try {
        (void)desktop_mutation(
            rejected, "defrag", "/tmp/engine", journal, 512, false);
        CHECK(false);
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) ==
               "qualified writer rejected feature X");
    }

    const auto args = desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, false);
    CHECK(args.at(0) == "/tmp/engine" && args.at(1) == "defrag");
    CHECK(args.at(7) == v.path && args.at(9) == journal);

    controls = desktop_controls(&v, false, false, true);
    CHECK(controls.defrag);
    CHECK(controls.growth_defrag);
    CHECK(controls.recover);
    try {
        (void)desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, true);
        CHECK(false);
    } catch (const std::runtime_error&) {}

    auto mounted = desktop_discover(Json::parse(
        R"({"blockdevices":[{"path":"/dev/sdc1","fstype":"ext4","size":1048576,"mountpoints":["/mnt/test"]}]})"));
    CHECK(mounted.size() == 1 && mounted.front().mounted);

    v.mounted = true;
    controls = desktop_controls(&v, false, false, false);
    CHECK(controls.unmount);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);
    try {
        (void)desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, false);
        CHECK(false);
    } catch (const std::runtime_error&) {}

    controls = desktop_controls(&v, true, false, false);
    CHECK(!controls.analyse);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(controls.stop);
}