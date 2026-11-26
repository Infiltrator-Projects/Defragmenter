// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/desktop_policy.hpp"
#include "../native/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(value) do { \
    if (!(value)) { \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
        std::exit(1); \
    } \
} while (0)

int main()
{
    using defragger::DesktopVolume;
    using defragger::Json;

    /*
     * Exercise the desktop/application boundary through its public value
     * contracts.  This deliberately does not include desktop.cpp, instantiate
     * GTK widgets, use a friend test, or reach into private Desktop state.
     * Presentation can now be reorganised without invalidating policy tests.
     */
    const Json lsblk = Json::parse(R"({
        "blockdevices": [
            {
                "path": "/dev/test1",
                "fstype": "ntfs",
                "label": "TEST",
                "uuid": "01234567-89ab-cdef-0123-456789abcdef",
                "partuuid": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                "serial": "SERIAL",
                "wwn": "WWN",
                "size": 1048576,
                "start": 2048,
                "ro": false,
                "mountpoints": [null]
            }
        ]
    })");

    auto volumes = defragger::desktop_discover(lsblk);
    CHECK(volumes.size() == 1U);
    DesktopVolume& volume = volumes.front();
    CHECK(volume.path == "/dev/test1");
    CHECK(volume.filesystem == "ntfs");
    CHECK(volume.label == "TEST");
    CHECK(!volume.mounted);
    CHECK(!volume.readonly);
    CHECK(!volume.verified);

    auto controls = defragger::desktop_controls(&volume, false, false, false);
    CHECK(controls.analyse);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);
    CHECK(!controls.stop);

    defragger::desktop_verify_identity(volume, "ntfs");
    CHECK(volume.verified);
    CHECK(!volume.exact_analysis);
    CHECK(!volume.defrag_qualified);
    CHECK(!volume.growth_qualified);

    /*
     * Once native identity has been proved for a writable offline target,
     * supported operations remain clickable even before writer qualification.
     * The mutation builder still owns fail-closed write authorisation.
     */
    controls = defragger::desktop_controls(&volume, false, false, false);
    CHECK(controls.analyse);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);

    volume.exact_analysis = true;
    volume.defrag_qualified = true;
    volume.growth_qualified = true;
    volume.recovery_qualified = true;
    controls = defragger::desktop_controls(&volume, false, false, false);
    CHECK(controls.analyse);
    CHECK(controls.defrag);
    CHECK(controls.growth_defrag);
    CHECK(controls.recover);
    const auto args=defragger::desktop_mutation(volume,"growth-defrag","engine","legacy.journal",4096,false);
    CHECK(args == std::vector<std::string>({"engine","growth-defrag","/dev/test1","--filesystem","ntfs","--write","--confirm","/dev/test1"}));

    controls = defragger::desktop_controls(&volume, true, false, false);
    CHECK(!controls.analyse);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);
    CHECK(controls.stop);

    controls = defragger::desktop_controls(&volume, false, false, true);
    CHECK(controls.analyse);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);

    volume.mounted = true;
    controls = defragger::desktop_controls(&volume, false, false, true);
    CHECK(controls.analyse);
    CHECK(controls.unmount);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);

    /* Test Media discovery labels are registry IDs/aliases, not a second catalogue. */
    const Json labelled = Json::parse(R"({
        "blockdevices": [
            {"path":"/dev/fat12","fstype":null,"partlabel":"LD_FAT12","size":1024,"ro":false,"mountpoints":[null]},
            {"path":"/dev/ext2","fstype":null,"partlabel":"LD_EXT2","size":1024,"ro":false,"mountpoints":[null]},
            {"path":"/dev/ofs","fstype":null,"partlabel":"LD_OFS","size":1024,"ro":false,"mountpoints":[null]},
            {"path":"/dev/hfsplus","fstype":null,"partlabel":"LD_HFSPLUS","size":1024,"ro":false,"mountpoints":[null]},
            {"path":"/dev/unknown","fstype":null,"partlabel":"LD_NOT_A_FILESYSTEM","size":1024,"ro":false,"mountpoints":[null]}
        ]
    })");
    const auto labelled_volumes = defragger::desktop_discover(labelled);
    CHECK(labelled_volumes.size() == 5U);
    CHECK(labelled_volumes[0].filesystem == "fat12");
    CHECK(labelled_volumes[1].filesystem == "ext2");
    CHECK(labelled_volumes[2].filesystem == "ofs");
    CHECK(labelled_volumes[3].filesystem == "hfsplus");

    CHECK(labelled_volumes[4].filesystem == "not_a_filesystem");
    // Generic descriptions are independent of installed provider names.
    CHECK(defragger::generic_operation("defrag").default_options.empty());
    CHECK(defragger::generic_operation("growth-defrag").default_options.empty());
    CHECK(defragger::generic_operation("recover").default_options.empty());
    return 0;
}