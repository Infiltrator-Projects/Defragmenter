// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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
    std::vector<std::string> default_options;
    std::vector<std::string> unsupported_options;
    bool raw_offline = true;
    bool live_updates = true;
};

enum class QualificationMode {
    None,
    WorkerPreflight,
    MapOrWorkerPreflight,
};

struct BackendInfo {
    std::string id;
    std::string display_name;
    std::vector<std::string> aliases;
    std::uint32_t capabilities = 0U;
    std::string map_accuracy;
    std::string worker;
    std::vector<OperationSpec> operations;
    QualificationMode qualification_mode = QualificationMode::None;
    std::string map_auxiliary_worker;
    std::vector<std::string> accepted_map_accuracies;

    BackendInfo(
        std::string id_value,
        std::string display_name_value,
        std::vector<std::string> aliases_value,
        std::uint32_t capabilities_value,
        std::string map_accuracy_value,
        std::string worker_value,
        std::vector<OperationSpec> operations_value,
        QualificationMode qualification_mode_value = QualificationMode::None,
        std::string map_auxiliary_worker_value = {},
        std::vector<std::string> accepted_map_accuracies_value = {})
        : id(std::move(id_value)),
          display_name(std::move(display_name_value)),
          aliases(std::move(aliases_value)),
          capabilities(capabilities_value),
          map_accuracy(std::move(map_accuracy_value)),
          worker(std::move(worker_value)),
          operations(std::move(operations_value)),
          qualification_mode(qualification_mode_value),
          map_auxiliary_worker(std::move(map_auxiliary_worker_value)),
          accepted_map_accuracies(std::move(accepted_map_accuracies_value)) {}
};

/* Generic UI description derived only from a Common V2 descriptor. */
BackendInfo backend_from_provider(std::string id,std::uint64_t defrag_flags);
OperationSpec generic_operation(std::string_view name);
const std::vector<BackendInfo>& backend_registry();
const BackendInfo* backend_by_fstype(std::string_view filesystem);
const OperationSpec* operation_for(const BackendInfo& backend,
                                   std::string_view operation);
bool backend_accepts_map_accuracy(const BackendInfo& backend,
                                  std::string_view accuracy);

std::vector<std::string> without_options(
    const std::vector<std::string>& arguments,
    const std::vector<std::string>& unsupported);

std::string resolve_program(std::string_view program_id,
                            bool allow_environment_override = false);
std::string json_quote(std::string_view value);
std::string registry_manifest_json(unsigned schema = 3U);

} // namespace defragger
