// SPDX-License-Identifier: GPL-3.0-or-later
#include "map.hpp"
#include "process.hpp"

#include <infiltratr/arithmetic.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace defragger {
namespace {

std::uint64_t multiply_or_throw(std::uint64_t left, std::uint64_t right,
                                const char* context) {
    std::uint64_t product = 0U;
    if (!infiltratr_u64_multiply_checked(left, right, &product)) {
        throw std::runtime_error(std::string(context) + " overflows uint64");
    }
    return product;
}

std::uint64_t cell_boundary(std::size_t index, std::uint64_t total,
                            std::size_t count) {
    if (count == 0U) throw std::logic_error("zero map-cell count");
    const std::uint64_t divisor = static_cast<std::uint64_t>(count);
    const std::uint64_t quotient = total / divisor;
    const std::uint64_t remainder = total % divisor;
    const std::uint64_t i = static_cast<std::uint64_t>(index);
    return i * quotient + (i * remainder) / divisor;
}

std::uint64_t required_u64(const Json& object, std::string_view key) {
    return object.at(key).unsigned_value();
}

std::uint64_t optional_u64(const Json& object, std::string_view key,
                           std::uint64_t fallback = 0U) {
    const Json* value = object.find(key);
    return value == nullptr ? fallback : value->unsigned_value();
}

double optional_real(const Json& object, std::string_view key,
                     double fallback = 0.0) {
    const Json* value = object.find(key);
    return value == nullptr ? fallback : value->real_value();
}

bool optional_bool(const Json& object, std::string_view key,
                   bool fallback = false) {
    const Json* value = object.find(key);
    return value == nullptr ? fallback : value->boolean();
}

std::string optional_string(const Json& object, std::string_view key,
                            std::string_view fallback = {}) {
    const Json* value = object.find(key);
    return value == nullptr ? std::string(fallback)
                            : std::string(value->string());
}

Json parse_worker_json(const CommandResult& result, const char* action) {
    if (result.return_code != 0) {
        const std::string detail = !result.standard_error.empty()
            ? result.standard_error : result.standard_output;
        throw std::runtime_error(
            std::string(action) + " failed" +
            (detail.empty() ? std::string{} : ": " + detail));
    }
    Json parsed = Json::parse(result.standard_output);
    if (!parsed.is_object())
        throw std::runtime_error(std::string(action) +
                                 " returned a non-object JSON value");
    return parsed;
}

CommandResult worker(
    const BackendInfo& backend,
    const std::string& mode,
    const std::string& path,
    const std::vector<std::string>& options = {},
    std::size_t output_limit = 64U * 1024U * 1024U,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) {
    std::vector<std::string> command{
        resolve_program(backend.worker), mode, path};
    command.insert(command.end(), options.begin(), options.end());
    CommandResult result = run_capture(command, output_limit, timeout,
        [](const std::string& line) {
            if (line.rfind("@@ANALYSIS ", 0U) == 0U) {
                std::fprintf(stderr, "%s\n", line.c_str());
                std::fflush(stderr);
            }
        });
    std::string diagnostics;
    for (std::size_t begin = 0U; begin < result.standard_error.size();) {
        const std::size_t newline = result.standard_error.find('\n', begin);
        const std::size_t end = newline == std::string::npos ? result.standard_error.size() : newline + 1U;
        if (result.standard_error.compare(begin, 11U, "@@ANALYSIS ") != 0)
            diagnostics.append(result.standard_error, begin, end - begin);
        begin = end;
    }
    result.standard_error = std::move(diagnostics);
    return result;
}

void analysis_phase(const char* phase) {
    const Json event(Json::Object{{"phase", Json(phase)}});
    std::fprintf(stderr, "@@ANALYSIS %s\n", event.dump().c_str());
    std::fflush(stderr);
}

struct MutationQualification {
    bool qualified = false;
    std::string reason;
};

bool backend_has_writer_preflight(const BackendInfo& backend) {
    return backend.id == "apfs" || backend.id == "btrfs" ||
           backend.id == "ext4" || backend.id == "hfs" ||
           backend.id == "hfsplus" || backend.id == "ntfs" ||
           backend.id == "ufs" || backend.id == "xfs";
}

std::string concise_child_detail(const CommandResult& result) {
    std::string detail = !result.standard_error.empty()
        ? result.standard_error : result.standard_output;
    while (!detail.empty() &&
           (detail.back() == '\n' || detail.back() == '\r'))
        detail.pop_back();
    constexpr std::size_t kMaxReason = 2048U;
    if (detail.size() > kMaxReason) {
        detail.resize(kMaxReason);
        detail += "...";
    }
    return detail;
}

MutationQualification mutation_qualification(
    const BackendInfo& backend, const std::string& path,
    const char* mode) {
    try {
        const CommandResult result = worker(
            backend, mode, path, {}, 2U * 1024U * 1024U,
            std::chrono::seconds(60));
        if (result.return_code == 0)
            return {true, {}};
        std::string detail = concise_child_detail(result);
        if (detail.empty())
            detail = "The native writer rejected this on-disk layout.";
        return {false, std::move(detail)};
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}

std::vector<UnitRange> pair_ranges(
    const Json& payload,
    std::string_view key,
    std::uint64_t total,
    bool required = false) {
    const Json* raw = payload.find(key);
    if (raw == nullptr) {
        if (required)
            throw std::runtime_error("native analyser omitted " +
                                     std::string(key));
        return {};
    }
    if (!raw->is_array())
        throw std::runtime_error("native analyser returned invalid " +
                                 std::string(key));

    std::vector<UnitRange> ranges;
    ranges.reserve(raw->array().size());
    for (const auto& value : raw->array()) {
        if (!value.is_array() || value.array().size() != 2U)
            throw std::runtime_error("native analyser returned malformed " +
                                     std::string(key));
        const std::uint64_t start = value.array()[0].unsigned_value();
        const std::uint64_t end = value.array()[1].unsigned_value();
        if (end <= start || end > total)
            throw std::runtime_error("native analyser returned out-of-range " +
                                     std::string(key));
        ranges.push_back({start, end});
    }
    return ranges;
}

Json::Object selected_details(
    const Json& payload,
    std::initializer_list<std::string_view> keys) {
    Json::Object details;
    for (const auto key : keys) {
        if (const Json* value = payload.find(key); value != nullptr)
            details.emplace(std::string(key), *value);
    }
    return details;
}

void set(Json& object, std::string key, Json value) {
    object.object()[std::move(key)] = std::move(value);
}

void add_fragmentation_summary(Json& result, const Json& payload,
                               bool copy_percentage) {
    for (const auto key : {
             std::string_view("regular_files"),
             std::string_view("directories"),
             std::string_view("fragmented_files"),
             std::string_view("fragmented_directories")}) {
        set(result, std::string(key),
            Json::unsigned_integer(optional_u64(payload, key)));
    }
    if (copy_percentage && payload.find("fragmentation_percent") != nullptr)
        set(result, "fragmentation_percent",
            Json::real(optional_real(payload, "fragmentation_percent")));
}

void overlay_fragmentation(Json& result, const Json& payload,
                           std::uint64_t total,
                           std::string_view units_name) {
    auto fragmented = pair_ranges(payload, "fragmented_ranges", total);
    auto directories = pair_ranges(payload, "directory_ranges", total);
    Json::Array& cells = result.at("cells").array();
    const std::uint64_t fragmented_units =
        overlay_ranges(cells, std::move(fragmented), "fragmented");
    const std::uint64_t directory_units =
        overlay_ranges(cells, std::move(directories), "directory");
    Json* details = result.find("details");
    if (details == nullptr) {
        set(result, "details", Json(Json::Object{}));
        details = result.find("details");
    }
    set(*details, "fragmented_" + std::string(units_name) + "_mapped",
        Json::unsigned_integer(fragmented_units));
    set(*details, "directory_" + std::string(units_name) + "_mapped",
        Json::unsigned_integer(directory_units));
}

Json generic_free_range_map(
    const BackendInfo& backend,
    const Json& payload,
    std::size_t cells,
    std::uint64_t total,
    std::uint64_t unit_size,
    std::string filesystem,
    Json::Object details,
    bool copy_percentage,
    std::string_view unit_name) {
    const auto free = pair_ranges(payload, "free_ranges", total, true);
    const auto used = complement_ranges(total, free);
    std::vector<StateRange> states;
    states.reserve(free.size() + used.size());
    for (const auto& range : free)
        states.push_back({range.start, range.end, 0U});
    for (const auto& range : used)
        states.push_back({range.start, range.end, 1U});

    Json result = aggregate_ranges(
        total, cells, unit_size, std::move(filesystem),
        std::move(states), backend.map_accuracy, std::move(details));
    add_fragmentation_summary(result, payload, copy_percentage);
    overlay_fragmentation(result, payload, total, unit_name);
    return result;
}

Json map_affs(const BackendInfo& backend, const std::string& path,
              std::size_t cells) {
    Json payload = parse_worker_json(
        worker(backend, "analyse-json", path), "native Amiga analyser");
    if (payload.at("filesystem").string() != "affs")
        throw std::runtime_error("native Amiga analyser returned wrong identity");
    const std::uint64_t total = required_u64(payload, "total_blocks");
    const std::uint64_t size = required_u64(payload, "block_size");
    Json::Object details;
    details["block_size"] = Json::unsigned_integer(size);
    details["dostype"] = Json(
        "DOS\\" + std::to_string(payload.find("dostype") != nullptr
            ? optional_u64(payload, "dostype") : 0U));
    details["variant"] = Json(payload.find("variant") != nullptr
        ? optional_string(payload, "variant", "Amiga DOS") : "Amiga DOS");
    details["fragmentation_available"] = Json(true);
    details["fragmentation_basis"] =
        Json("first-party native C OFS/FFS block catalogue");
    return generic_free_range_map(
        backend, payload, cells, total, size, "affs",
        std::move(details), false, "blocks");
}

Json map_exfat(const BackendInfo& backend, const std::string& path,
               std::size_t cells) {
    Json payload = parse_worker_json(
        worker(backend, "analyse-json", path), "native exFAT analyser");
    if (payload.at("filesystem").string() != "exfat")
        throw std::runtime_error("native exFAT analyser returned wrong identity");
    const std::uint64_t size = required_u64(payload, "cluster_size");
    const std::uint64_t total = required_u64(payload, "total_clusters");
    Json::Object details =
        selected_details(payload, {"cluster_size", "serial"});
    details["fragmentation_available"] = Json(true);
    details["fragmentation_basis"] =
        Json("native C exFAT FAT/bitmap/directory catalogue");
    Json result = generic_free_range_map(
        backend, payload, cells, total, size, "exfat",
        std::move(details), true, "clusters");
    if (const Json* value = payload.find("growth_10_satisfied"))
        set(result, "growth_10_satisfied", *value);
    return result;
}

Json map_ext(const BackendInfo& backend, const std::string& path,
             std::size_t cells) {
    Json payload = parse_worker_json(
        worker(backend, "analyse-json", path), "native EXT analyser");
    const std::string filesystem(payload.at("filesystem").string());
    if (filesystem != "ext2" && filesystem != "ext3" &&
        filesystem != "ext4") {
        throw std::runtime_error("native EXT analyser returned wrong identity");
    }
    const std::uint64_t size = required_u64(payload, "block_size");
    const std::uint64_t total = required_u64(payload, "total_blocks");

    const BackendInfo metadata_backend{
        "ext-metadata", "EXT metadata", {}, 0U, "exact",
        "ext-metadata", MapAdapter::NativeMap, {}};
    Json metadata = parse_worker_json(
        worker(metadata_backend, "analyse-json", path),
        "native EXT metadata classifier");
    if (required_u64(metadata, "block_size") != size ||
        required_u64(metadata, "total_blocks") != total) {
        throw std::runtime_error(
            "native EXT metadata classifier returned mismatched geometry");
    }

    Json::Object details;
    details["fragmentation_available"] = Json(true);
    details["fragmentation_basis"] =
        Json("native C EXT inode allocation scanner");
    details["metadata_basis"] =
        Json("native C EXT block-group and reserved-inode classifier");
    for (const auto key : {"inodes_scanned", "malformed_inodes"})
        if (const Json* value = payload.find(key)) details[key] = *value;
    if (const Json* value = payload.find("growth_10_satisfied"))
        details["growth_10_satisfied"] = *value;

    Json result = generic_free_range_map(
        backend, payload, cells, total, size, filesystem,
        std::move(details), false, "blocks");

    if (const Json* warning = payload.find("analysis_warning")) {
        if (!warning->is_string()) throw std::runtime_error("EXT analysis warning is not text");
        set(result, "analysis_warning", *warning);
    }

    auto metadata_ranges =
        pair_ranges(metadata, "metadata_ranges", total, true);
    const std::uint64_t metadata_units =
        overlay_ranges(result.at("cells").array(),
                       std::move(metadata_ranges), "bad");
    set(result.at("details"), "metadata_blocks_mapped",
        Json::unsigned_integer(metadata_units));

    const std::uint64_t regular =
        payload.find("regular_files") != nullptr
            ? optional_u64(payload, "regular_files") : 0U;
    const std::uint64_t fragmented =
        payload.find("fragmented_files") != nullptr
            ? optional_u64(payload, "fragmented_files") : 0U;
    set(result, "fragmentation_percent",
        Json::real(100.0 * static_cast<double>(fragmented) /
                   static_cast<double>(std::max<std::uint64_t>(1U, regular))));
    return result;
}

Json map_fat(const BackendInfo& backend, const std::string& path,
             std::size_t cells) {
    Json payload = parse_worker_json(
        worker(backend, "map", path,
               {"--cells", std::to_string(std::max<std::size_t>(1U, cells))},
               map_capture_limit(cells)),
        "native FAT mapper");

    const Json* filesystem = payload.find("filesystem");
    if (filesystem == nullptr || !filesystem->is_string()) {
        throw std::runtime_error(
            "native FAT mapper omitted filesystem identity");
    }
    const BackendInfo* resolved = backend_by_fstype(filesystem->string());
    if (resolved == nullptr || resolved->id != backend.id) {
        throw std::runtime_error(
            "native FAT mapper returned wrong filesystem identity");
    }

    const std::uint64_t bytes_per_sector =
        required_u64(payload, "bytes_per_sector");
    const std::uint64_t sectors_per_cluster =
        required_u64(payload, "sectors_per_cluster");
    const std::uint64_t cluster_size = required_u64(payload, "cluster_size");
    const std::uint64_t total_sectors =
        required_u64(payload, "total_sectors");
    const std::uint64_t data_start_sector =
        required_u64(payload, "data_start_sector");
    const std::uint64_t total = required_u64(payload, "data_clusters");
    const std::uint64_t declared_free = required_u64(payload, "free_clusters");
    const std::uint64_t cell_count = required_u64(payload, "cell_count");
    const Json* raw_cells = payload.find("cells");
    if (bytes_per_sector == 0U || sectors_per_cluster == 0U ||
        cluster_size == 0U || total_sectors == 0U || total == 0U ||
        declared_free > total ||
        multiply_or_throw(bytes_per_sector, sectors_per_cluster,
                          "FAT cluster geometry") != cluster_size ||
        cell_count == 0U || raw_cells == nullptr || !raw_cells->is_array() ||
        cell_count != raw_cells->array().size() ||
        total > std::numeric_limits<std::uint64_t>::max() - 2U) {
        throw std::runtime_error(
            "native FAT mapper returned invalid map geometry");
    }
    const std::uint64_t data_sectors =
        multiply_or_throw(total, sectors_per_cluster, "FAT data sectors");
    if (data_start_sector > total_sectors ||
        data_sectors > total_sectors - data_start_sector) {
        throw std::runtime_error(
            "native FAT mapper returned invalid physical sector geometry");
    }
    const std::uint64_t trailing_sectors =
        total_sectors - data_start_sector - data_sectors;

    std::uint64_t expected_start = 2U;
    std::uint64_t free_total = 0U;
    std::uint64_t used_total = 0U;
    for (const auto& cell : raw_cells->array()) {
        if (!cell.is_object()) {
            throw std::runtime_error(
                "native FAT mapper returned malformed allocation cell");
        }
        const std::uint64_t start = required_u64(cell, "start");
        const std::uint64_t end = required_u64(cell, "end");
        const std::uint64_t free = required_u64(cell, "free");
        const std::uint64_t used = required_u64(cell, "used");
        const std::uint64_t fragmented = required_u64(cell, "fragmented");
        const std::uint64_t directory = required_u64(cell, "directory");
        const std::uint64_t bad = required_u64(cell, "bad");
        if (start != expected_start || end < start ||
            end >= total + 2U) {
            throw std::runtime_error(
                "native FAT mapper returned discontinuous allocation cells");
        }
        const std::uint64_t span = end - start + 1U;
        if (free > span || used > span || free + used != span ||
            fragmented > used || directory > used || bad > used) {
            throw std::runtime_error(
                "native FAT mapper returned inconsistent allocation counts");
        }
        free_total += free;
        used_total += used;
        expected_start = end + 1U;
    }
    if (expected_start != total + 2U ||
        free_total != declared_free ||
        used_total != total - declared_free) {
        throw std::runtime_error(
            "native FAT mapper totals do not match filesystem geometry");
    }

    /*
     * The FAT worker deliberately emits its native FAT-specific contract
     * (cluster_size/data_clusters/free_clusters and cluster-number cell
     * bounds starting at 2).  The C++ desktop consumes the common allocation
     * map contract used by every other adapter.  Preserve all FAT-specific
     * fields and cells, but add the common geometry/totals here so the GUI
     * does not need a second FAT-only presenter.
     */
    set(payload, "schema", Json::unsigned_integer(1U));
    set(payload, "backend", Json("read-only-domain"));
    set(payload, "map_accuracy", Json(backend.map_accuracy));
    set(payload, "unit_size", Json::unsigned_integer(cluster_size));
    set(payload, "total_units", Json::unsigned_integer(total));
    set(payload, "display_unit_size",
        Json::unsigned_integer(bytes_per_sector));
    set(payload, "display_units_per_allocation_unit",
        Json::unsigned_integer(sectors_per_cluster));
    set(payload, "display_prefix_units",
        Json::unsigned_integer(data_start_sector));
    set(payload, "display_suffix_units",
        Json::unsigned_integer(trailing_sectors));
    set(payload, "display_total_units",
        Json::unsigned_integer(total_sectors));
    set(payload, "display_unit_name", Json("sectors"));
    set(payload, "total_bytes", Json::unsigned_integer(
        multiply_or_throw(total, cluster_size, "FAT capacity")));
    set(payload, "free_bytes", Json::unsigned_integer(
        multiply_or_throw(declared_free, cluster_size, "FAT free space")));
    set(payload, "used_bytes", Json::unsigned_integer(
        multiply_or_throw(total - declared_free, cluster_size,
                          "FAT used space")));
    set(payload, "unknown_bytes", Json::unsigned_integer(0U));
    set(payload, "outside_bytes", Json::unsigned_integer(0U));
    set(payload, "filesystem_bytes", Json::unsigned_integer(
        multiply_or_throw(total, cluster_size, "FAT filesystem size")));

    const std::uint64_t regular_files =
        optional_u64(payload, "regular_files");
    const std::uint64_t fragmented_files =
        optional_u64(payload, "fragmented_files");
    set(payload, "fragmentation_percent", Json::real(
        regular_files == 0U
            ? 0.0
            : 100.0 * static_cast<double>(fragmented_files) /
                  static_cast<double>(regular_files)));

    Json::Object details;
    details["bytes_per_sector"] =
        Json::unsigned_integer(bytes_per_sector);
    details["sectors_per_cluster"] =
        Json::unsigned_integer(sectors_per_cluster);
    details["cluster_size"] = Json::unsigned_integer(cluster_size);
    details["total_sectors"] = Json::unsigned_integer(total_sectors);
    details["data_start_sector"] =
        Json::unsigned_integer(data_start_sector);
    details["data_clusters"] = Json::unsigned_integer(total);
    details["free_clusters"] = Json::unsigned_integer(declared_free);
    details["fragmentation_available"] = Json(true);
    details["fragmentation_basis"] =
        Json("native C FAT directory catalogue and cluster-chain scan");
    set(payload, "details", Json(std::move(details)));

    return payload;
}

Json map_hfsplus(const BackendInfo& backend, const std::string& path,
                  std::size_t cells) {
    Json payload = parse_worker_json(
        worker(backend, "analyse-json", path), "native HFS+ analyser");
    if (payload.at("filesystem").string() != "hfsplus")
        throw std::runtime_error("native HFS+ analyser returned wrong identity");
    const std::uint64_t total = required_u64(payload, "total_blocks");
    const std::uint64_t size = required_u64(payload, "block_size");
    Json::Object details;
    details["block_size"] = Json::unsigned_integer(size);
    details["variant"] = Json(payload.find("variant") != nullptr
        ? optional_string(payload, "variant", "HFS+") : "HFS+");
    details["journaled"] = Json(payload.find("journaled") != nullptr
        ? optional_bool(payload, "journaled") : false);
    details["fragmentation_available"] = Json(true);
    details["fragmentation_basis"] =
        Json("first-party native C HFS+/HFSX catalog and allocation-file scan");
    Json result = generic_free_range_map(
        backend, payload, cells, total, size,
        payload.find("signature") != nullptr &&
                payload.at("signature").string() == "HX"
            ? "hfsx" : "hfsplus",
        std::move(details), false, "blocks");
    if (const Json* value = payload.find("free_blocks"))
        set(result.at("details"), "header_free_blocks", *value);
    return result;
}

Json map_ntfs(const BackendInfo& backend, const std::string& path,
              std::size_t cells) {
    Json payload = parse_worker_json(
        worker(backend, "analyse-json", path, {"--qualify"}), "native NTFS analyser");
    if (payload.at("filesystem").string() != "ntfs")
        throw std::runtime_error("native NTFS analyser returned wrong identity");
    const std::uint64_t size = required_u64(payload, "cluster_size");
    const std::uint64_t total = required_u64(payload, "total_clusters");
    Json::Object details =
        selected_details(payload, {"cluster_size", "serial",
                                   "mft_records_scanned",
                                   "mft_malformed_records",
                                   "hibernation_active"});
    details["fragmentation_available"] = Json(true);
    details["fragmentation_basis"] =
        Json("native C NTFS MFT and mapping-pairs catalogue");
    Json result = generic_free_range_map(
        backend, payload, cells, total, size, "ntfs",
        std::move(details), true, "clusters");
    if (const Json* value = payload.find("growth_10_satisfied"))
        set(result, "growth_10_satisfied", *value);
    const Json* defrag = payload.find("defrag_qualified");
    const Json* growth = payload.find("growth_qualified");
    if ((defrag == nullptr) != (growth == nullptr))
        throw std::runtime_error("NTFS analyser returned incomplete qualification");
    if (defrag != nullptr) {
        if (!defrag->is_bool() || !growth->is_bool())
            throw std::runtime_error("NTFS qualification is not boolean");
        set(result, "defrag_qualified", *defrag);
        set(result, "growth_qualified", *growth);
    }
    for (const auto key : {"defrag_reason", "growth_reason"}) {
        if (const Json* value = payload.find(key)) {
            if (!value->is_string()) throw std::runtime_error("NTFS qualification reason is not text");
            set(result, key, *value);
        }
    }
    return result;
}

Json map_xfs(const BackendInfo& backend, const std::string& path,
             std::size_t cells) {
    Json payload = parse_worker_json(
        worker(backend, "analyse-json", path), "native XFS analyser");
    if (payload.at("filesystem").string() != "xfs")
        throw std::runtime_error("native XFS analyser returned wrong identity");
    const std::uint64_t size = required_u64(payload, "block_size");
    const std::uint64_t total = required_u64(payload, "dblocks");
    Json::Object details =
        selected_details(payload, {
            "block_size", "sector_size", "inode_size", "agblocks",
            "allocation_groups", "bnobt_blocks", "fdblocks",
            "inodes_scanned", "malformed_inodes", "realtime_inodes",
            "inobt_blocks", "bmap_blocks"});
    if (const auto found = details.find("agblocks"); found != details.end()) {
        details["allocation_group_blocks"] = found->second;
        details.erase(found);
    }
    if (const auto found = details.find("fdblocks"); found != details.end()) {
        details["superblock_free_blocks"] = found->second;
        details.erase(found);
    }
    if (const auto found = details.find("realtime_inodes");
        found != details.end()) {
        details["realtime_inodes_not_mapped"] = found->second;
        details.erase(found);
    }
    details["free_space_basis"] =
        Json("XFS per-allocation-group block-number free-space B+trees");
    details["fragmentation_available"] = Json(true);
    details["fragmentation_basis"] =
        Json("native C XFS inode B+tree and data-fork extent scanner");
    return generic_free_range_map(
        backend, payload, cells, total, size, "xfs",
        std::move(details), true, "blocks");
}

bool native_accuracy_matches(const BackendInfo& backend,
                             std::string_view accuracy) {
    if (backend.id == "btrfs") return accuracy == "exact-single-device";
    if (backend.id == "swap")
        return accuracy == "summary" || accuracy == "exact";
    if (backend.id == "zfs")
        return accuracy == "summary" || accuracy == "exact";
    if (backend.id == "ufs")
        return accuracy == "summary" || accuracy == "exact-allocation" ||
               accuracy == "exact";
    return accuracy == backend.map_accuracy;
}

void validate_native_map(const BackendInfo& backend, const Json& payload) {
    if (payload.find("schema") == nullptr ||
        payload.at("schema").unsigned_value() != 1U)
        throw std::runtime_error(
            "native filesystem mapper returned incompatible map schema");

    const Json* domain = payload.find("backend");
    if (domain == nullptr || !domain->is_string() ||
        domain->string() != "read-only-domain")
        throw std::runtime_error(
            "native filesystem mapper returned wrong backend domain");

    const Json* filesystem = payload.find("filesystem");
    if (filesystem == nullptr || !filesystem->is_string())
        throw std::runtime_error(
            "native filesystem mapper omitted filesystem identity");
    const BackendInfo* resolved = backend_by_fstype(filesystem->string());
    if (resolved == nullptr || resolved->id != backend.id)
        throw std::runtime_error(
            "native filesystem mapper returned wrong filesystem identity");

    const Json* accuracy = payload.find("map_accuracy");
    if (accuracy == nullptr || !accuracy->is_string() ||
        !native_accuracy_matches(backend, accuracy->string()))
        throw std::runtime_error(
            "native filesystem mapper returned unexpected accuracy contract");

    const Json* raw_cells = payload.find("cells");
    if (raw_cells == nullptr || !raw_cells->is_array())
        throw std::runtime_error(
            "native filesystem mapper omitted allocation cells");

    const std::uint64_t unit_size = required_u64(payload, "unit_size");
    const std::uint64_t total_units = required_u64(payload, "total_units");
    const std::uint64_t cell_count = required_u64(payload, "cell_count");
    if (unit_size == 0U || total_units == 0U || cell_count == 0U ||
        cell_count != raw_cells->array().size())
        throw std::runtime_error(
            "native filesystem mapper returned invalid map geometry");

    std::uint64_t expected_start = 0U;
    for (const auto& cell : raw_cells->array()) {
        if (!cell.is_object())
            throw std::runtime_error(
                "native filesystem mapper returned malformed allocation cell");
        const std::uint64_t start = required_u64(cell, "start");
        const std::uint64_t end = required_u64(cell, "end");
        if (start != expected_start || end < start || end >= total_units)
            throw std::runtime_error(
                "native filesystem mapper returned discontinuous allocation cells");

        const std::uint64_t span = end - start + 1U;
        const std::uint64_t free = required_u64(cell, "free");
        const std::uint64_t used = required_u64(cell, "used");
        const std::uint64_t unknown =
            optional_u64(cell, "unknown");
        const std::uint64_t outside =
            optional_u64(cell, "outside");
        const std::uint64_t bad =
            optional_u64(cell, "bad");
        const std::uint64_t fragmented =
            optional_u64(cell, "fragmented");
        const std::uint64_t directory =
            optional_u64(cell, "directory");

        if (free > span || used > span || unknown > span ||
            outside > span || bad > span ||
            fragmented > used || directory > used ||
            free > UINT64_MAX - used ||
            free + used > UINT64_MAX - unknown ||
            free + used + unknown > UINT64_MAX - outside ||
            free + used + unknown + outside > UINT64_MAX - bad ||
            free + used + unknown + outside + bad != span) {
            throw std::runtime_error(
                "native filesystem mapper returned inconsistent allocation counts");
        }
        expected_start = end + 1U;
    }
    if (expected_start != total_units)
        throw std::runtime_error(
            "native filesystem mapper did not cover the complete map");
}

Json map_native(const BackendInfo& backend, const std::string& path,
                std::size_t cells) {
    const CommandResult result =
        worker(backend, "map", path,
               {"--cells", std::to_string(std::max<std::size_t>(1U, cells))},
               map_capture_limit(cells));
    Json payload = parse_worker_json(result, "native filesystem mapper");
    validate_native_map(backend, payload);
    std::string warning = result.standard_error;
    while (!warning.empty() &&
           (warning.back() == '\n' || warning.back() == '\r'))
        warning.pop_back();
    constexpr std::size_t kMaxAnalysisWarning = 2048U;
    if (warning.size() > kMaxAnalysisWarning) {
        warning.resize(kMaxAnalysisWarning);
        warning += "...";
    }
    if (!warning.empty())
        set(payload, "analysis_warning", Json(std::move(warning)));
    return payload;
}

} // namespace

std::size_t map_capture_limit(std::size_t cells) {
    if (cells == 0U || cells > kMaxMapCells)
        throw std::invalid_argument("map-cell count is outside the supported range");
    // Native cell records contain bounded numeric fields. Reserve 256 bytes
    // per cell plus the established 64 MiB allowance for metadata and details.
    return 64U * 1024U * 1024U + cells * 256U;
}

std::vector<UnitRange> merge_ranges(std::vector<UnitRange> ranges) {
    std::sort(ranges.begin(), ranges.end(),
              [](const UnitRange& left, const UnitRange& right) {
                  if (left.start != right.start) return left.start < right.start;
                  return left.end < right.end;
              });
    std::vector<UnitRange> merged;
    for (const auto& range : ranges) {
        if (range.end <= range.start) continue;
        if (!merged.empty() && range.start <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

std::vector<UnitRange> complement_ranges(
    std::uint64_t total_units,
    const std::vector<UnitRange>& occupied) {
    std::vector<UnitRange> gaps;
    std::uint64_t cursor = 0U;
    for (auto range : merge_ranges(occupied)) {
        range.start = std::min(total_units, range.start);
        range.end = std::min(total_units, range.end);
        if (range.start > cursor) gaps.push_back({cursor, range.start});
        cursor = std::max(cursor, range.end);
        if (cursor >= total_units) break;
    }
    if (cursor < total_units) gaps.push_back({cursor, total_units});
    return gaps;
}

Json aggregate_ranges(
    std::uint64_t total_units,
    std::size_t cell_count,
    std::uint64_t unit_size,
    std::string filesystem,
    std::vector<StateRange> ranges,
    std::string accuracy,
    Json::Object details) {
    if (total_units == 0U)
        throw std::runtime_error("allocation-unit count must be positive");
    cell_count = std::max<std::size_t>(
        1U, std::min<std::size_t>(
                cell_count, static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        total_units,
                        std::numeric_limits<std::size_t>::max()))));

    std::sort(ranges.begin(), ranges.end(),
              [](const StateRange& left, const StateRange& right) {
                  if (left.start != right.start) return left.start < right.start;
                  if (left.end != right.end) return left.end < right.end;
                  return left.state < right.state;
              });
    std::vector<StateRange> ordered;
    for (auto range : ranges) {
        range.start = std::min(range.start, total_units);
        range.end = std::min(range.end, total_units);
        if (range.end <= range.start) continue;
        if (range.state > 3U)
            throw std::runtime_error("invalid allocation state");
        if (!ordered.empty() && range.start < ordered.back().end)
            throw std::runtime_error("overlapping allocation-state ranges");
        if (!ordered.empty() && range.start == ordered.back().end &&
            range.state == ordered.back().state) {
            ordered.back().end = range.end;
        } else {
            ordered.push_back(range);
        }
    }

    std::uint64_t totals[4]{0U, 0U, 0U, 0U};
    Json::Array result_cells;
    result_cells.reserve(cell_count);
    std::size_t range_index = 0U;

    for (std::size_t index = 0U; index < cell_count; ++index) {
        const std::uint64_t start =
            cell_boundary(index, total_units, cell_count);
        const std::uint64_t end =
            cell_boundary(index + 1U, total_units, cell_count);
        std::uint64_t counts[4]{0U, 0U, 0U, 0U};
        std::uint64_t cursor = start;

        while (range_index < ordered.size() &&
               ordered[range_index].end <= start) {
            ++range_index;
        }
        std::size_t check = range_index;
        while (check < ordered.size() && ordered[check].start < end) {
            const auto& range = ordered[check];
            const std::uint64_t overlap_start =
                std::max(start, range.start);
            const std::uint64_t overlap_end =
                std::min(end, range.end);
            if (overlap_start > cursor)
                counts[2] += overlap_start - cursor;
            if (overlap_end > overlap_start) {
                counts[range.state] += overlap_end - overlap_start;
                cursor = std::max(cursor, overlap_end);
            }
            if (range.end > end) break;
            ++check;
        }
        if (cursor < end) counts[2] += end - cursor;
        for (unsigned state = 0U; state < 4U; ++state)
            totals[state] += counts[state];

        Json::Object cell;
        cell["start"] = Json::unsigned_integer(start);
        cell["end"] = Json::unsigned_integer(end - 1U);
        cell["free"] = Json::unsigned_integer(counts[0]);
        cell["used"] = Json::unsigned_integer(counts[1]);
        cell["unknown"] = Json::unsigned_integer(counts[2]);
        cell["bad"] = Json::unsigned_integer(counts[3]);
        cell["fragmented"] = Json::unsigned_integer(0U);
        cell["directory"] = Json::unsigned_integer(0U);
        result_cells.emplace_back(std::move(cell));
    }

    Json::Object result;
    result["schema"] = Json::unsigned_integer(1U);
    result["backend"] = Json("read-only-domain");
    result["filesystem"] = Json(std::move(filesystem));
    result["map_accuracy"] = Json(std::move(accuracy));
    result["unit_size"] = Json::unsigned_integer(unit_size);
    result["total_units"] = Json::unsigned_integer(total_units);
    result["cell_count"] =
        Json::unsigned_integer(static_cast<std::uint64_t>(cell_count));
    result["total_bytes"] =
        Json::unsigned_integer(multiply_or_throw(
            total_units, unit_size, "filesystem capacity"));
    result["free_bytes"] =
        Json::unsigned_integer(multiply_or_throw(
            totals[0], unit_size, "free-space total"));
    result["used_bytes"] =
        Json::unsigned_integer(multiply_or_throw(
            totals[1], unit_size, "used-space total"));
    result["unknown_bytes"] =
        Json::unsigned_integer(multiply_or_throw(
            totals[2] + totals[3], unit_size, "unknown-space total"));
    result["cells"] = Json(std::move(result_cells));
    if (!details.empty()) result["details"] = Json(std::move(details));
    return Json(std::move(result));
}

std::uint64_t overlay_ranges(
    Json::Array& cells,
    std::vector<UnitRange> ranges,
    std::string_view field) {
    const auto merged = merge_ranges(std::move(ranges));
    std::uint64_t total = 0U;
    for (const auto& range : merged) total += range.end - range.start;

    std::size_t range_index = 0U;
    for (auto& cell : cells) {
        const std::uint64_t start = cell.at("start").unsigned_value();
        const std::uint64_t end = cell.at("end").unsigned_value() + 1U;
        while (range_index < merged.size() &&
               merged[range_index].end <= start) {
            ++range_index;
        }
        std::uint64_t overlap = 0U;
        std::size_t check = range_index;
        while (check < merged.size() && merged[check].start < end) {
            overlap += std::max<std::uint64_t>(
                0U, std::min(end, merged[check].end) -
                    std::max(start, merged[check].start));
            if (merged[check].end > end) break;
            ++check;
        }
        const std::uint64_t used =
            optional_u64(cell, "used");
        set(cell, std::string(field),
            Json::unsigned_integer(std::min(used, overlap)));
    }
    return total;
}

std::string backend_identified_filesystem(
    const BackendInfo& backend,
    const std::string& path) {
    try {
        const Json payload = parse_worker_json(
            worker(
                backend, "identify", path, {}, 1024U * 1024U,
                std::chrono::seconds(30)),
            "filesystem identifier");
        const Json* filesystem = payload.find("filesystem");
        if (filesystem == nullptr || !filesystem->is_string()) return {};
        std::string identified(filesystem->string());
        for (char& byte : identified) {
            if (byte >= 'A' && byte <= 'Z')
                byte = static_cast<char>(byte - 'A' + 'a');
        }
        const BackendInfo* resolved = backend_by_fstype(identified);
        if (resolved == nullptr || resolved->id != backend.id) return {};
        return identified;
    } catch (...) {
        return {};
    }
}

bool backend_probe(const BackendInfo& backend, const std::string& path) {
    return !backend_identified_filesystem(backend, path).empty();
}

Json map_backend(const BackendInfo& backend, const std::string& path,
                 std::size_t cells) {
    (void)map_capture_limit(cells);
    analysis_phase("Scanning filesystem allocation and file metadata");
    Json result;
    switch (backend.map_adapter) {
    case MapAdapter::NativeMap:
        result = map_native(backend, path, cells);
        break;
    case MapAdapter::Affs:
        result = map_affs(backend, path, cells);
        break;
    case MapAdapter::Exfat:
        result = map_exfat(backend, path, cells);
        break;
    case MapAdapter::Ext:
        result = map_ext(backend, path, cells);
        break;
    case MapAdapter::Fat:
        result = map_fat(backend, path, cells);
        break;
    case MapAdapter::HfsPlus:
        result = map_hfsplus(backend, path, cells);
        break;
    case MapAdapter::Ntfs:
        result = map_ntfs(backend, path, cells);
        break;
    case MapAdapter::Xfs:
        result = map_xfs(backend, path, cells);
        break;
    default:
        throw std::runtime_error("unknown allocation-map adapter");
    }

    if ((backend.capabilities & CAP_DEFRAG) != 0U) {
        MutationQualification defrag{true, {}};
        MutationQualification growth{true, {}};
        if (backend_has_writer_preflight(backend) &&
            !(backend.id == "ntfs" && result.find("defrag_qualified") &&
              result.find("growth_qualified"))) {
            analysis_phase("Checking Defragment availability (up to 60 seconds)");
            defrag = mutation_qualification(
                backend, path, "preflight-defrag");
            analysis_phase("Checking Growth Defrag availability (up to 60 seconds)");
            growth = mutation_qualification(
                backend, path, "preflight-growth");
        }
        if (!result.find("defrag_qualified"))
            set(result, "defrag_qualified", Json(defrag.qualified));
        if (!result.find("growth_qualified"))
            set(result, "growth_qualified", Json(growth.qualified));
        if (!defrag.reason.empty())
            set(result, "defrag_reason", Json(std::move(defrag.reason)));
        if (!growth.reason.empty())
            set(result, "growth_reason", Json(std::move(growth.reason)));
    }
    analysis_phase("Preparing allocation map");
    return result;
}

} // namespace defragger
