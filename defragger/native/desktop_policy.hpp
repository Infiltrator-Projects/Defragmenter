// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace defragger {

struct DesktopVolume {
    std::string path, filesystem, label, filesystem_uuid, partition_uuid;
    std::uint64_t size = 0;
    bool mounted = false, readonly = false, image = false, verified = false;
    bool exact_analysis = false;
};

struct DesktopControls {
    bool analyse = false, unmount = false, defrag = false;
    bool growth_defrag = false, recover = false, stop = false;
};

std::vector<DesktopVolume> desktop_discover(const Json& lsblk);
DesktopControls desktop_controls(const DesktopVolume* volume, bool busy,
                                 bool stopping, bool journal_exists);
std::string desktop_journal(const DesktopVolume& volume, unsigned uid);
std::vector<std::string> desktop_mutation(const DesktopVolume& volume,
                                          std::string_view operation,
                                          std::string_view engine,
                                          std::string_view journal,
                                          unsigned cells,
                                          bool journal_exists);
void desktop_verify_identity(DesktopVolume& volume, std::string_view detected);

} // namespace defragger
