// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_policy.hpp"

#include <cassert>
#include <stdexcept>

int main() {
    using namespace defragger;
    auto volumes = desktop_discover(Json::parse(R"({"blockdevices":[{"path":"/dev/sdb","children":[{"path":"/dev/sdb1","fstype":"ext4","size":1048576,"mountpoints":[null],"uuid":"UUID","partuuid":"PART"}]}]})"));
    assert(volumes.size() == 1);
    auto& v = volumes.front();
    assert(v.path == "/dev/sdb1" && !v.verified && !v.mounted);

    auto labelled = desktop_discover(Json::parse(
        R"({"blockdevices":[{"path":"/dev/mmcblk0","children":[{"path":"/dev/mmcblk0p1","fstype":null,"label":"LD_FAT12","partlabel":"LD_FAT12","size":267386880,"mountpoints":[null]}]}]})"));
    assert(labelled.size() == 1);
    assert(labelled.front().path == "/dev/mmcblk0p1");
    assert(labelled.front().filesystem == "fat12");
    assert(labelled.front().label == "LD_FAT12");
    assert(!labelled.front().mounted);
    assert(desktop_controls(&v, false, false, false).analyse);
    assert(!desktop_controls(&v, false, false, false).defrag);
    assert(desktop_journal(v, 1000) ==
        "/var/lib/linux-defragger/state/1000/volume-fc1f33e16c4a68d6.journal");
    try {
        desktop_verify_identity(v, "ntfs");
        assert(false);
    } catch (const std::runtime_error&) {}
    assert(!v.verified);
    desktop_verify_identity(v, "ext4");
    assert(v.verified);
    assert(!desktop_controls(&v, false, false, false).defrag);
    v.exact_analysis = true;
    assert(!desktop_controls(&v, false, false, false).defrag);
    v.defrag_qualified = true;
    v.growth_qualified = true;
    assert(desktop_controls(&v, false, false, false).defrag);
    assert(desktop_controls(&v, false, false, false).growth_defrag);
    DesktopVolume fat = v;
    fat.filesystem = "vfat";
    fat.verified = false;
    desktop_verify_identity(fat, "FAT16");
    assert(fat.verified && fat.filesystem == "fat16" &&
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
    assert(desktop_journal(removable, 1000) == stable_journal);
    const auto journal = desktop_journal(v, 1000);
    DesktopVolume rejected = v;
    rejected.defrag_qualified = false;
    rejected.defrag_reason = "qualified writer rejected feature X";
    try {
        (void)desktop_mutation(
            rejected, "defrag", "/tmp/engine", journal, 512, false);
        assert(false);
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()) ==
               "qualified writer rejected feature X");
    }
    const auto args = desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, false);
    assert(args.at(0) == "/tmp/engine" && args.at(1) == "defrag");
    assert(args.at(7) == v.path && args.at(9) == journal);
    try {
        (void)desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, true);
        assert(false);
    } catch (const std::runtime_error&) {}
    auto mounted = desktop_discover(Json::parse(
        R"({"blockdevices":[{"path":"/dev/sdc1","fstype":"ext4","size":1048576,"mountpoints":["/mnt/test"]}]})"));
    assert(mounted.size() == 1 && mounted.front().mounted);

    v.mounted = true;
    assert(desktop_controls(&v, false, false, false).unmount);
    assert(!desktop_controls(&v, false, false, false).defrag);
    try {
        (void)desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, false);
        assert(false);
    } catch (const std::runtime_error&) {}
}
