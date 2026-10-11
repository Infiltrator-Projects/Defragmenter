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

    volume.exact_analysis = true;
    volume.defrag_qualified = true;
    volume.growth_qualified = true;
    controls = defragger::desktop_controls(&volume, false, false, false);
    CHECK(controls.analyse);
    CHECK(controls.defrag);
    CHECK(controls.growth_defrag);
    CHECK(!controls.recover);

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
    CHECK(controls.recover);

    volume.mounted = true;
    controls = defragger::desktop_controls(&volume, false, false, true);
    CHECK(controls.analyse);
    CHECK(controls.unmount);
    CHECK(!controls.defrag);
    CHECK(!controls.growth_defrag);
    CHECK(!controls.recover);

    /* The runtime registry, not GTK, remains the capability authority. */
    const auto* backend = defragger::backend_by_fstype("ntfs");
    CHECK(backend != nullptr);
    CHECK(defragger::operation_for(*backend, "defrag") != nullptr);
    CHECK(defragger::operation_for(*backend, "growth-defrag") != nullptr);
    CHECK(defragger::operation_for(*backend, "recover") != nullptr);

    /* Worker invocation defaults belong to operation contracts, not desktop policy. */
    const auto* fat = defragger::backend_by_fstype("fat12");
    const auto* ext = defragger::backend_by_fstype("ext4");
    CHECK(fat != nullptr);
    CHECK(ext != nullptr);
    const auto* fat_defrag = defragger::operation_for(*fat, "defrag");
    const auto* fat_recover = defragger::operation_for(*fat, "recover");
    const auto* ext_defrag = defragger::operation_for(*ext, "defrag");
    CHECK(fat_defrag != nullptr);
    CHECK(fat_recover != nullptr);
    CHECK(ext_defrag != nullptr);
    CHECK(fat_defrag->default_options ==
          std::vector<std::string>({"--batch-clusters", "4096",
                                    "--ram-buffer", "auto"}));
    CHECK(fat_recover->default_options.empty());
    CHECK(ext_defrag->default_options ==
          std::vector<std::string>({"--batch-clusters", "4096"}));

    return 0;
}
