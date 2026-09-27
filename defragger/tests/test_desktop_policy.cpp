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
    auto unknown = desktop_probe_candidates(Json::parse(R"({"blockdevices":[
      {"path":"/dev/sdc","children":[{"path":"/dev/sdc1","partlabel":"LD_OFS","size":2097152}]},
      {"path":"/dev/sdd1","fstype":"ext4"}]})"));
    assert(unknown.size() == 2);
    assert(unknown[1].path == "/dev/sdc1" && unknown[1].filesystem == "ofs");
    assert(desktop_controls(&v, false, false, false).analyse);
    assert(!desktop_controls(&v, false, false, false).defrag);
    assert(desktop_journal(v, 1000) ==
        "/var/lib/linux-defragger/state/1000/dev_sdb1-35f2914ce8df5666.journal");
    try {
        desktop_verify_identity(v, "ntfs");
        assert(false);
    } catch (const std::runtime_error&) {}
    assert(!v.verified);
    desktop_verify_identity(v, "ext4");
    assert(v.verified && desktop_controls(&v, false, false, false).defrag);
    DesktopVolume fat = v;
    fat.filesystem = "vfat";
    fat.verified = false;
    desktop_verify_identity(fat, "fat16");
    assert(fat.verified && fat.filesystem == "fat16");
    const auto journal = desktop_journal(v, 1000);
    const auto args = desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, false);
    assert(args.at(0) == "/tmp/engine" && args.at(1) == "defrag");
    assert(args.at(7) == v.path && args.at(9) == journal);
    try {
        (void)desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, true);
        assert(false);
    } catch (const std::runtime_error&) {}
    v.mounted = true;
    assert(desktop_controls(&v, false, false, false).unmount);
    assert(!desktop_controls(&v, false, false, false).defrag);
    try {
        (void)desktop_mutation(v, "defrag", "/tmp/engine", journal, 512, false);
        assert(false);
    } catch (const std::runtime_error&) {}
}
