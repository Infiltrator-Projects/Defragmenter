// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace defragger {

enum Capability : std::uint32_t {
    CAP_ANALYSE = 1U << 0U,
    CAP_MAP = 1U << 1U,
    CAP_DEFRAG = 1U << 2U,
    CAP_RECOVER = 1U << 3U,
    CAP_LIVE_MAP = 1U << 4U,
    CAP_GROWTH_DEFRAG = 1U << 5U,
};

struct OperationSpec {
    std::string name;
    std::string worker;
    std::string layout_policy;
    std::string label;
    std::string description;
    std::string warning;
    std::vector<std::string> unsupported_options;
    bool raw_offline = true;
    bool live_updates = true;
};

enum class MapAdapter {
    NativeMap,
    Affs,
    Exfat,
    Ext,
    Fat,
    HfsPlus,
    Ntfs,
    Xfs,
};

struct BackendInfo {
    std::string id;
    std::string display_name;
    std::vector<std::string> aliases;
    std::uint32_t capabilities = 0U;
    std::string map_accuracy;
    std::string worker;
    MapAdapter map_adapter = MapAdapter::NativeMap;
    std::vector<OperationSpec> operations;
};

const std::vector<BackendInfo>& backend_registry();
const BackendInfo* backend_by_fstype(std::string_view filesystem);
const OperationSpec* operation_for(const BackendInfo& backend,
                                   std::string_view operation);

std::vector<std::string> without_options(
    const std::vector<std::string>& arguments,
    const std::vector<std::string>& unsupported);

std::string resolve_program(std::string_view program_id);
std::string json_quote(std::string_view value);
std::string registry_manifest_json(unsigned schema = 3U);

} // namespace defragger
