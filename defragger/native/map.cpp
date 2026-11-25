// SPDX-License-Identifier: GPL-3.0-or-later
#include "map.hpp"
extern "C" {
#include "filesystem_module_host.h"
}

#include <infiltratr/arithmetic.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

bool optional_bool(const Json& object, std::string_view key,
                   bool fallback = false) {
    const Json* value = object.find(key);
    return value == nullptr ? fallback : value->boolean();
}

void analysis_phase(const char* phase) {
    const Json event(Json::Object{{"phase", Json(phase)}});
    std::fprintf(stderr, "@@ANALYSIS %s\n", event.dump().c_str());
    std::fflush(stderr);
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

void set(Json& object, std::string key, Json value) {
    object.object()[std::move(key)] = std::move(value);
}

Json map_fat(const BackendInfo& backend,const Json& payload,std::size_t cells) {
    // The SO returns authoritative allocation and file extents. Pixel
    // aggregation is a presentation operation; no BPB or FAT is read here.
    if (payload.at("filesystem").string() != backend.id ||
        required_u64(payload,"abi") != 2U)
        throw std::runtime_error("FAT provider returned wrong identity or ABI");
    const auto unit=required_u64(payload,"unit_size");
    const auto total=required_u64(payload,"total_units");
    const auto free=required_u64(payload,"free_units");
    if (!unit || !total || free>total)
        throw std::runtime_error("invalid Common V2 FAT geometry");
    const auto occupied=merge_ranges(pair_ranges(payload,"allocated_ranges",total,true));
    auto fragmented=merge_ranges(pair_ranges(payload,"fragmented_ranges",total,true));
    std::uint64_t allocated=0;
    for(const auto& r:occupied) allocated+=r.end-r.start;
    if(allocated!=total-free)
        throw std::runtime_error("FAT provider allocation and free-space totals disagree");
    std::size_t cursor=0;
    for(const auto& r:fragmented) {
        while(cursor<occupied.size() && occupied[cursor].end<=r.start)++cursor;
        if(cursor==occupied.size() || occupied[cursor].start>r.start ||
           occupied[cursor].end<r.end)
            throw std::runtime_error("FAT provider marked unallocated data as fragmented");
    }
    std::vector<StateRange> ranges;
    for(const auto& r:occupied)ranges.push_back({r.start,r.end,1U});
    for(const auto& r:complement_ranges(total,occupied))ranges.push_back({r.start,r.end,0U});
    Json result=aggregate_ranges(total,cells,unit,backend.id,std::move(ranges),
        backend.map_accuracy,{{"fragmentation_available",Json(true)},
        {"fragmentation_basis",Json("Common V2 provider file extent catalogue")}});
    overlay_ranges(result.at("cells").array(),std::move(fragmented),"fragmented");
    for(const auto* field:{"regular_files","fragmented_files","directories",
           "fragmented_directories","directory_fragmentation_available",
           "defrag_qualified","growth_qualified","recovery_qualified"}) {
        if(const Json* value=payload.find(field))set(result,field,*value);
    }
    if(!optional_bool(payload,"defrag_qualified"))
        set(result,"defrag_reason",Json("Common V2 provider did not qualify a read-only Defragment plan"));
    if(!optional_bool(payload,"growth_qualified"))
        set(result,"growth_reason",Json("Common V2 provider did not qualify a read-only Growth Defrag plan"));
    const auto regular=required_u64(payload,"regular_files");
    const auto split=required_u64(payload,"fragmented_files");
    if(split>regular)throw std::runtime_error("invalid Common V2 fragmentation totals");
    set(result,"fragmentation_percent",Json::real(regular?100.0*static_cast<double>(split)/static_cast<double>(regular):0.0));
    set(result,"data_clusters",Json::unsigned_integer(total));
    set(result,"free_clusters",Json::unsigned_integer(free));
    set(result,"cluster_size",Json::unsigned_integer(unit));
    set(result,"outside_bytes",Json::unsigned_integer(0U));
    // Common V2 supplies physical byte offsets when a volume has allocations.
    // An empty FAT12/16 data area has no physical extent from which to infer
    // the prefix; omit rather than invent geometry in that case.
    if(optional_bool(payload,"physical_geometry_available")) {
        const auto prefix=required_u64(payload,"data_start_bytes");
        const auto bytes=required_u64(payload,"device_bytes");
        const auto data=multiply_or_throw(unit,total,"FAT capacity");
        if(prefix>bytes || data>bytes-prefix)
            throw std::runtime_error("invalid Common V2 physical extent geometry");
        set(result,"display_unit_size",Json::unsigned_integer(1U));
        set(result,"display_units_per_allocation_unit",Json::unsigned_integer(unit));
        set(result,"display_prefix_units",Json::unsigned_integer(prefix));
        set(result,"display_suffix_units",Json::unsigned_integer(bytes-prefix-data));
        set(result,"display_total_units",Json::unsigned_integer(bytes));
        set(result,"display_unit_name",Json("bytes"));
    }
    return result;
}

/* All non-FAT providers use the same Common V2 per-file extent contract.
 * The host has no authoritative free-space or directory allocation map;
 * unclassified physical bytes remain unknown, so a partial map cannot
 * authorize destructive operations in the desktop. */
Json map_catalogue(const BackendInfo& backend,const Json& payload,
                   std::size_t cells) {
    const auto filesystem=std::string(payload.at("filesystem").string());
    const auto* actual=backend_by_fstype(filesystem);
    if((filesystem!=backend.id && (!actual || actual->id!=backend.id)) || required_u64(payload,"abi")!=2U)
        throw std::runtime_error("Common V2 catalogue has wrong identity or ABI");
    const auto total=required_u64(payload,"device_bytes");
    if(!total)throw std::runtime_error("Common V2 catalogue has no target geometry");
    std::vector<UnitRange> occupied,fragmented;
    for(const auto& extent:payload.at("file_extents").array()) {
        if(!extent.is_array() || extent.array().size()!=3U)
            throw std::runtime_error("Malformed Common V2 file extent");
        const auto start=extent.array()[0].unsigned_value();
        const auto end=extent.array()[1].unsigned_value();
        const auto split=extent.array()[2].unsigned_value();
        if(start>=end || end>total || split>1U)
            throw std::runtime_error("Invalid Common V2 physical extent");
        occupied.push_back({start,end});
        if(split)fragmented.push_back({start,end});
    }
    std::vector<StateRange> ranges;
    std::uint64_t reserved=0U, allocated=0U, unknown=0U, free=0U;
    const auto* physical=payload.find("allocation_ranges");
    if(physical) {
        // Revalidate the transport before presentation. Canonical provider
        // allocation is a complete partition coverage, not a list of file
        // extents. Reserved bytes are unavailable (rendered as used), but
        // separately reported. Unknown bytes never become free.
        std::uint64_t cursor=0U;
        std::vector<UnitRange> free_ranges;
        for(const auto& value:physical->array()) {
            if(!value.is_array() || value.array().size()!=3U)
                throw std::runtime_error("Malformed Common V2 allocation range");
            const auto start=value.array()[0].unsigned_value();
            const auto end=value.array()[1].unsigned_value();
            const auto state=value.array()[2].unsigned_value();
            if(start!=cursor || end<=start || end>total || state>IFS_FS_SPACE_RESERVED)
                throw std::runtime_error("Invalid Common V2 allocation coverage");
            const auto length=end-start;
            unsigned display=2U;
            if(state==IFS_FS_SPACE_FREE) {
                free+=length; display=0U; free_ranges.push_back({start,end});
            } else if(state==IFS_FS_SPACE_ALLOCATED) {
                allocated+=length;display=1U;
            } else if(state==IFS_FS_SPACE_RESERVED) {
                reserved+=length;display=1U;
            } else unknown+=length;
            ranges.push_back({start,end,display});
            cursor=end;
        }
        if(cursor!=total || free!=required_u64(payload,"allocation_free_bytes") ||
           unknown!=required_u64(payload,"allocation_unknown_bytes"))
            throw std::runtime_error("Incomplete or inconsistent Common V2 allocation coverage");
        std::size_t i=0U;
        for(const auto& r:merge_ranges(occupied)) {
            while(i<free_ranges.size() && free_ranges[i].end<=r.start)++i;
            if(i<free_ranges.size() && free_ranges[i].start<r.end)
                throw std::runtime_error("Common V2 file extent overlaps provider-confirmed free space");
        }
    } else {
        for(const auto& r:merge_ranges(std::move(occupied)))
            ranges.push_back({r.start,r.end,1U});
    }
    Json result=aggregate_ranges(total,cells,1U,filesystem,std::move(ranges),
        physical && !unknown ? "exact-allocation" : "summary",
        {{"fragmentation_available",Json(true)},
        {"fragmentation_basis",Json("Common V2 file extent catalogue")}});
    if(physical) {
        set(result,"reserved_bytes",Json::unsigned_integer(reserved));
        set(result,"allocated_bytes",Json::unsigned_integer(allocated));
    }
    overlay_ranges(result.at("cells").array(),std::move(fragmented),"fragmented");
    for(const auto* key:{"regular_files","fragmented_files","directories",
                         "fragmented_directories","directory_fragmentation_available",
                         "provider_defrag_capabilities","recovery_qualified"})
        if(const Json* value=payload.find(key))set(result,key,*value);
    const auto regular=required_u64(payload,"regular_files");
    const auto split=required_u64(payload,"fragmented_files");
    if(split>regular)throw std::runtime_error("Invalid Common V2 file totals");
    set(result,"fragmentation_percent",Json::real(regular?
        100.0*static_cast<double>(split)/static_cast<double>(regular):0.0));
    // Allocation accuracy and writer qualification are separate contracts.
    // Only a real provider PLAN on the read-only mount may qualify either
    // operation when the physical allocation map remains incomplete.
    const bool preflight=optional_bool(payload,"provider_plan_preflight");
    const bool packed=preflight && optional_bool(payload,"defrag_qualified");
    const bool growth=preflight && optional_bool(payload,"growth_qualified");
    set(result,"provider_plan_preflight",Json(preflight));
    set(result,"defrag_qualified",Json(packed));
    set(result,"growth_qualified",Json(growth));
    if(!packed)set(result,"defrag_reason",Json("Common V2 provider did not qualify a read-only Defragment plan"));
    if(!growth)set(result,"growth_reason",Json("Common V2 provider did not qualify a read-only Growth Defrag plan"));
    return result;
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
    const BackendInfo& backend,const std::string& path,std::string_view exact) {
    // Explicit variants cannot be silently substituted by sibling formats.
    std::vector<const char *> candidates{backend.id.c_str()};
    if(backend.id=="affs")candidates={"ofs","ffs"};
    else if(backend.id=="ufs")candidates={"ufs1","ufs2"};
    else if(backend.id=="sfs")candidates={"sfs","sfs2"};
    std::string selected(exact);
    if(!selected.empty()) {
        bool allowed=false;
        for(const auto* id:candidates)if(selected==id)allowed=true;
        if(!allowed)return {};
        candidates={selected.c_str()};
    }
    char format[IFS_FS_MAX_FORMAT_ID+1u]={0},module[64]={0};
    const int result=ifs_filesystem_module_detect_v2(path.c_str(),
        candidates.data(),candidates.size(),format,sizeof(format),module,sizeof(module));
    if(result==-2)throw std::runtime_error("Ambiguous Common V2 provider identity");
    return result==0?std::string(format):std::string{};
}

bool backend_probe(const BackendInfo& backend, const std::string& path) {
    return !backend_identified_filesystem(backend, path).empty();
}

/* Pure presentation contract, also exercised by regression tests. */
Json map_v2_response(const BackendInfo& backend,const Json& payload,std::size_t cells) {
    (void)map_capture_limit(cells);
    return backend.map_adapter==MapAdapter::Fat
        ? map_fat(backend,payload,cells) : map_catalogue(backend,payload,cells);
}

Json map_backend(const BackendInfo& backend, const std::string& path,
                 std::size_t cells,std::string_view exact) {
    (void)map_capture_limit(cells);
    analysis_phase("Scanning filesystem allocation and file metadata");
    const auto format=backend_identified_filesystem(backend,path,exact);
    if(format.empty())throw std::runtime_error("No unique trusted Common V2 provider recognises this target");
    char *raw=nullptr;
    if(ifs_filesystem_module_capture_map_v2(format.c_str(),path.c_str(),
          &raw,map_capture_limit(cells))!=0 || !raw)
        throw std::runtime_error("Common V2 provider could not supply a complete file extent catalogue");
    std::unique_ptr<char,decltype(&std::free)> json(raw,&std::free);
    Json result=map_v2_response(backend,Json::parse(json.get()),cells);
    analysis_phase("Preparing allocation map");
    return result;
}

} // namespace defragger