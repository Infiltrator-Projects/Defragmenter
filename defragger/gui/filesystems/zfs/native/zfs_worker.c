// SPDX-License-Identifier: GPL-3.0-or-later
#include "zfs_native.h"
#include "version.h"
#include "infiltratr/core.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG "linux-defragger-zfs-worker"
#define ZFS_MAP_UNIT_SIZE 512U

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT\n",
                  PROG);
}

static int parse_cells(const char *text, uint64_t *cells)
{
    return infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, cells)
        ? 0 : -1;
}

static void print_summary_json(const LdZfsSummary *summary, int detailed)
{
    (void)printf("{\"filesystem\":\"zfs\",\"byte_order\":\"%s\"",
                 zfs_byte_order_name(summary));
    if (detailed != 0) {
        (void)printf(
            ",\"map_accuracy\":\"summary\","
            "\"size_bytes\":%llu,\"label_index\":%u,"
            "\"uberblock_slot\":%u,\"uberblock_magic_offset\":%llu,"
            "\"uberblock_version\":%llu,\"uberblock_txg\":%llu,"
            "\"uberblock_guid_sum\":%llu,\"uberblock_timestamp\":%llu,"
            "\"candidate_uberblocks\":%u,"
            "\"root_vdev\":%llu,\"root_offset\":%llu,"
            "\"root_asize\":%llu,\"root_lsize\":%llu,\"root_psize\":%llu,"
            "\"root_logical_birth\":%llu,\"root_compression\":%u,"
            "\"root_checksum\":%u,\"root_type\":%u,\"root_level\":%u,"
            "\"root_embedded\":%s,\"config_known\":%s,"
            "\"single_leaf_supported\":%s,\"pool_guid\":%llu,"
            "\"leaf_guid\":%llu,\"top_guid\":%llu,\"top_vdev_id\":%llu,"
            "\"ashift\":%llu,\"metaslab_array\":%llu,"
            "\"metaslab_shift\":%llu,\"top_vdev_asize\":%llu,"
            "\"metaslab_vdevs\":%u,\"top_vdev_type\":\"%s\"",
            (unsigned long long)summary->size_bytes,
            summary->label_index,
            summary->uberblock_slot,
            (unsigned long long)summary->uberblock_magic_offset,
            (unsigned long long)summary->uberblock_version,
            (unsigned long long)summary->uberblock_txg,
            (unsigned long long)summary->uberblock_guid_sum,
            (unsigned long long)summary->uberblock_timestamp,
            summary->candidate_uberblocks,
            (unsigned long long)summary->root_vdev,
            (unsigned long long)summary->root_offset,
            (unsigned long long)summary->root_asize,
            (unsigned long long)summary->root_lsize,
            (unsigned long long)summary->root_psize,
            (unsigned long long)summary->root_logical_birth,
            summary->root_compression,
            summary->root_checksum,
            summary->root_type,
            summary->root_level,
            summary->root_embedded ? "true" : "false",
            summary->config_known ? "true" : "false",
            summary->single_leaf_supported ? "true" : "false",
            (unsigned long long)summary->pool_guid,
            (unsigned long long)summary->leaf_guid,
            (unsigned long long)summary->top_guid,
            (unsigned long long)summary->top_vdev_id,
            (unsigned long long)summary->ashift,
            (unsigned long long)summary->metaslab_array,
            (unsigned long long)summary->metaslab_shift,
            (unsigned long long)summary->top_vdev_asize,
            summary->metaslab_vdevs,
            summary->top_vdev_type);
    }
    (void)puts("}");
}

static void print_exact_analysis_json(const LdZfsSummary *summary,
                                      const LdZfsAnalysis *analysis)
{
    (void)printf(
        "{\"filesystem\":\"zfs\",\"byte_order\":\"%s\","
        "\"map_accuracy\":\"exact\",\"size_bytes\":%llu,"
        "\"free_bytes\":%llu,\"used_bytes\":%llu,\"unknown_bytes\":%llu,"
        "\"regular_files\":%llu,\"directories\":0,"
        "\"fragmented_files\":%llu,\"fragmented_directories\":0,"
        "\"fragmented_bytes\":%llu,\"allocated_extents\":%llu,"
        "\"uberblock_version\":%llu,\"uberblock_txg\":%llu,"
        "\"top_vdev_id\":%llu,\"ashift\":%llu,"
        "\"metaslab_array\":%llu,\"metaslab_shift\":%llu,"
        "\"top_vdev_asize\":%llu}\n",
        zfs_byte_order_name(summary),
        (unsigned long long)analysis->size_bytes,
        (unsigned long long)analysis->free_bytes,
        (unsigned long long)analysis->used_bytes,
        (unsigned long long)analysis->unknown_bytes,
        (unsigned long long)analysis->files_seen,
        (unsigned long long)analysis->fragmented_files,
        (unsigned long long)analysis->fragmented_bytes,
        (unsigned long long)analysis->allocated_extents,
        (unsigned long long)summary->uberblock_version,
        (unsigned long long)summary->uberblock_txg,
        (unsigned long long)summary->top_vdev_id,
        (unsigned long long)summary->ashift,
        (unsigned long long)summary->metaslab_array,
        (unsigned long long)summary->metaslab_shift,
        (unsigned long long)summary->top_vdev_asize);
}

static void print_summary_map(const LdZfsSummary *summary,
                              uint64_t requested_cells)
{
    uint64_t total_units = summary->size_bytes / ZFS_MAP_UNIT_SIZE;
    if (summary->size_bytes % ZFS_MAP_UNIT_SIZE != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;

    uint64_t cell_count = requested_cells;
    if (cell_count > total_units)
        cell_count = total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    const uint64_t rounded_bytes = total_units * ZFS_MAP_UNIT_SIZE;
    (void)printf("{\"schema\":1,\"backend\":\"read-only-domain\","
                 "\"filesystem\":\"zfs\",\"map_accuracy\":\"summary\","
                 "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
                 "\"total_bytes\":%llu,\"free_bytes\":0,\"used_bytes\":0,"
                 "\"unknown_bytes\":%llu,\"regular_files\":0,"
                 "\"directories\":0,\"fragmented_files\":0,"
                 "\"fragmented_directories\":0,\"cells\":[",
                 ZFS_MAP_UNIT_SIZE,
                 (unsigned long long)total_units,
                 (unsigned long long)cell_count,
                 (unsigned long long)rounded_bytes,
                 (unsigned long long)rounded_bytes);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = (index * total_units) / cell_count;
        const uint64_t end_exclusive =
            ((index + 1U) * total_units) / cell_count;
        const uint64_t length = end_exclusive - start;
        if (index != 0U)
            (void)putchar(',');
        (void)printf("{\"start\":%llu,\"end\":%llu,\"free\":0,\"used\":0,"
                     "\"unknown\":%llu,\"bad\":0,\"fragmented\":0,"
                     "\"directory\":0}",
                     (unsigned long long)start,
                     (unsigned long long)(end_exclusive - 1U),
                     (unsigned long long)length);
    }

    (void)printf(
        "],\"details\":{\"uberblock_magic_offset\":%llu,"
        "\"uberblock_txg\":%llu,\"uberblock_version\":%llu,"
        "\"label_index\":%u,\"uberblock_slot\":%u,"
        "\"candidate_uberblocks\":%u,\"byte_order\":\"%s\","
        "\"mos_root\":{\"vdev\":%llu,\"offset\":%llu,\"asize\":%llu,"
        "\"lsize\":%llu,\"psize\":%llu,\"birth_txg\":%llu,"
        "\"compression\":%u,\"checksum\":%u,\"type\":%u,\"level\":%u,"
        "\"embedded\":%s},"
        "\"topology\":{\"known\":%s,\"single_leaf_supported\":%s,"
        "\"pool_guid\":%llu,\"leaf_guid\":%llu,\"top_guid\":%llu,"
        "\"top_vdev_id\":%llu,\"ashift\":%llu,\"metaslab_array\":%llu,"
        "\"metaslab_shift\":%llu,\"top_vdev_asize\":%llu,"
        "\"metaslab_vdevs\":%u,\"type\":\"%s\"},"
        "\"label_basis\":\"four OpenZFS leaf-vdev labels and 128-entry uberblock rings\","
        "\"note\":\"Exact native allocation and file-fragmentation analysis is bounded to qualified single-disk legacy pool versions 1-28; other OpenZFS feature sets remain summary-only\"}}\n",
        (unsigned long long)summary->uberblock_magic_offset,
        (unsigned long long)summary->uberblock_txg,
        (unsigned long long)summary->uberblock_version,
        summary->label_index,
        summary->uberblock_slot,
        summary->candidate_uberblocks,
        zfs_byte_order_name(summary),
        (unsigned long long)summary->root_vdev,
        (unsigned long long)summary->root_offset,
        (unsigned long long)summary->root_asize,
        (unsigned long long)summary->root_lsize,
        (unsigned long long)summary->root_psize,
        (unsigned long long)summary->root_logical_birth,
        summary->root_compression,
        summary->root_checksum,
        summary->root_type,
        summary->root_level,
        summary->root_embedded ? "true" : "false",
        summary->config_known ? "true" : "false",
        summary->single_leaf_supported ? "true" : "false",
        (unsigned long long)summary->pool_guid,
        (unsigned long long)summary->leaf_guid,
        (unsigned long long)summary->top_guid,
        (unsigned long long)summary->top_vdev_id,
        (unsigned long long)summary->ashift,
        (unsigned long long)summary->metaslab_array,
        (unsigned long long)summary->metaslab_shift,
        (unsigned long long)summary->top_vdev_asize,
        summary->metaslab_vdevs,
        summary->top_vdev_type);
}

static uint64_t range_overlap_units(const LdZfsAnalysis *analysis,
                                    uint64_t cell_start,
                                    uint64_t cell_end,
                                    uint32_t required_flags)
{
    uint64_t total = 0U;
    for (size_t index = 0U; index < analysis->range_count; ++index) {
        const LdZfsRange *range = &analysis->ranges[index];
        if ((range->flags & required_flags) != required_flags)
            continue;
        if (range->start > UINT64_MAX - range->length)
            continue;
        const uint64_t range_end = range->start + range->length;
        const uint64_t start =
            range->start > cell_start ? range->start : cell_start;
        const uint64_t end = range_end < cell_end ? range_end : cell_end;
        if (end > start)
            total += (end - start) / ZFS_MAP_UNIT_SIZE;
    }
    return total;
}

static void print_exact_map(const LdZfsSummary *summary,
                            const LdZfsAnalysis *analysis,
                            uint64_t requested_cells)
{
    uint64_t total_units = analysis->size_bytes / ZFS_MAP_UNIT_SIZE;
    if (analysis->size_bytes % ZFS_MAP_UNIT_SIZE != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;

    uint64_t cell_count = requested_cells;
    if (cell_count > total_units)
        cell_count = total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    (void)printf(
        "{\"schema\":1,\"backend\":\"read-only-domain\","
        "\"filesystem\":\"zfs\",\"map_accuracy\":\"exact\","
        "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
        "\"total_bytes\":%llu,\"free_bytes\":%llu,\"used_bytes\":%llu,"
        "\"unknown_bytes\":0,\"regular_files\":%llu,\"directories\":0,"
        "\"fragmented_files\":%llu,\"fragmented_directories\":0,"
        "\"cells\":[",
        ZFS_MAP_UNIT_SIZE,
        (unsigned long long)total_units,
        (unsigned long long)cell_count,
        (unsigned long long)analysis->size_bytes,
        (unsigned long long)analysis->free_bytes,
        (unsigned long long)analysis->used_bytes,
        (unsigned long long)analysis->files_seen,
        (unsigned long long)analysis->fragmented_files);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = (index * total_units) / cell_count;
        const uint64_t end_exclusive =
            ((index + 1U) * total_units) / cell_count;
        const uint64_t cell_start = start * ZFS_MAP_UNIT_SIZE;
        uint64_t cell_end = end_exclusive * ZFS_MAP_UNIT_SIZE;
        if (cell_end > analysis->size_bytes)
            cell_end = analysis->size_bytes;
        const uint64_t length = end_exclusive - start;
        const uint64_t used =
            range_overlap_units(analysis, cell_start, cell_end,
                                LD_ZFS_RANGE_ALLOCATED);
        const uint64_t fragmented =
            range_overlap_units(analysis, cell_start, cell_end,
                                LD_ZFS_RANGE_FRAGMENTED);
        const uint64_t free = length >= used ? length - used : 0U;

        if (index != 0U)
            (void)putchar(',');
        (void)printf(
            "{\"start\":%llu,\"end\":%llu,\"free\":%llu,\"used\":%llu,"
            "\"unknown\":0,\"bad\":0,\"fragmented\":%llu,\"directory\":0}",
            (unsigned long long)start,
            (unsigned long long)(end_exclusive - 1U),
            (unsigned long long)free,
            (unsigned long long)used,
            (unsigned long long)fragmented);
    }

    (void)printf(
        "],\"details\":{\"uberblock_version\":%llu,\"uberblock_txg\":%llu,"
        "\"top_vdev_id\":%llu,\"ashift\":%llu,"
        "\"metaslab_array\":%llu,\"metaslab_shift\":%llu,"
        "\"top_vdev_asize\":%llu,\"allocated_extents\":%llu,"
        "\"fragmented_bytes\":%llu,"
        "\"allocation_basis\":\"MOS metaslab_array plus replayed per-metaslab space maps\","
        "\"fragmentation_basis\":\"head-dataset plain-file dnode block-pointer trees\","
        "\"bounded_subset\":\"single top-level disk, legacy pool version 1-28, verified Fletcher4/off checksums and off/LZJB/LZ4 metadata compression\"}}\n",
        (unsigned long long)summary->uberblock_version,
        (unsigned long long)summary->uberblock_txg,
        (unsigned long long)summary->top_vdev_id,
        (unsigned long long)summary->ashift,
        (unsigned long long)summary->metaslab_array,
        (unsigned long long)summary->metaslab_shift,
        (unsigned long long)summary->top_vdev_asize,
        (unsigned long long)analysis->allocated_extents,
        (unsigned long long)analysis->fragmented_bytes);
}

static int analyse_exact_or_summary(const char *path,
                                    const LdZfsSummary *summary,
                                    int json_only)
{
    LdZfsAnalysis analysis;
    char error[256] = {0};
    if (zfs_analyse_exact(path, &analysis, error, sizeof(error)) == 0) {
        if (json_only != 0)
            print_exact_analysis_json(summary, &analysis);
        zfs_analysis_destroy(&analysis);
        return 0;
    }
    if (errno == ENOTSUP) {
        if (json_only != 0)
            print_summary_json(summary, 1);
        return 0;
    }
    (void)fprintf(stderr, "%s: %s\n", PROG,
                  error[0] != '\0' ? error : strerror(errno));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }

    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t requested_cells = 0U;
        if (parse_cells(argv[4], &requested_cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        LdZfsSummary summary;
        char error[256] = {0};
        if (zfs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }

        LdZfsAnalysis analysis;
        if (zfs_analyse_exact(argv[2], &analysis,
                              error, sizeof(error)) == 0) {
            print_exact_map(&summary, &analysis, requested_cells);
            zfs_analysis_destroy(&analysis);
            return 0;
        }
        if (errno == ENOTSUP) {
            print_summary_map(&summary, requested_cells);
            return 0;
        }
        (void)fprintf(stderr, "%s: %s\n", PROG,
                      error[0] != '\0' ? error : strerror(errno));
        return 1;
    }

    if (argc != 3) {
        usage(stderr);
        return 2;
    }

    LdZfsSummary summary;
    char error[256] = {0};
    if (zfs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    if (strcmp(argv[1], "identify") == 0) {
        print_summary_json(&summary, 0);
        return 0;
    }
    if (strcmp(argv[1], "analyse-json") == 0)
        return analyse_exact_or_summary(argv[2], &summary, 1);

    usage(stderr);
    return 2;
}
