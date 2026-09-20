// SPDX-License-Identifier: GPL-3.0-or-later
#include "apfs_native.h"
#include "version.h"

#include "infiltratr/core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG "linux-defragger-apfs-worker"

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
            "map DEVICE --cells COUNT\n", PROG);
}

static void print_uuid(const uint8_t uuid[16])
{
    for (size_t index = 0U; index < 16U; ++index)
        printf("%02x", (unsigned int)uuid[index]);
}

static uint64_t overlap(const ApfsRange *ranges, size_t count,
                        uint64_t start, uint64_t end)
{
    uint64_t total = 0U;
    for (size_t index = 0U; index < count; ++index) {
        if (ranges[index].end <= start)
            continue;
        if (ranges[index].start >= end)
            break;
        const uint64_t left =
            ranges[index].start > start ? ranges[index].start : start;
        const uint64_t right =
            ranges[index].end < end ? ranges[index].end : end;
        if (right > left)
            total += right - left;
    }
    return total;
}

static void print_analysis(const ApfsAnalysis *analysis)
{
    const double percent = infiltratr_percent_u64(
        analysis->fragmented_files, analysis->regular_files);
    printf("{\"filesystem\":\"apfs\",\"block_size\":%u,\"block_count\":%" PRIu64
           ",\"container_uuid\":\"",
           analysis->block_size, analysis->block_count);
    print_uuid(analysis->container_uuid);
    printf("\",\"xid\":%" PRIu64 ",\"active_nx_block\":%" PRIu64
           ",\"spaceman_block\":%" PRIu64 ",\"volume_oid\":%" PRIu64
           ",\"volume_super_block\":%" PRIu64 ",\"free_blocks\":%" PRIu64
           ",\"used_blocks\":%" PRIu64 ",\"regular_files\":%" PRIu64
           ",\"directories\":%" PRIu64 ",\"fragmented_files\":%" PRIu64
           ",\"fragmented_directories\":0,\"fragmentation_percent\":%.6f}\n",
           analysis->xid, analysis->active_nx_block, analysis->spaceman_block,
           analysis->volume_oid, analysis->volume_super_block,
           analysis->free_blocks, analysis->used_blocks,
           analysis->regular_files, analysis->directories,
           analysis->fragmented_files, percent);
}

static int print_map(const ApfsAnalysis *analysis, uint64_t requested_cells)
{
    uint64_t cells = requested_cells;
    if (cells > analysis->block_count)
        cells = analysis->block_count;
    if (cells == 0U)
        cells = 1U;
    uint64_t free_total = 0U;
    uint64_t used_total = 0U;
    printf("{\"schema\":1,\"backend\":\"read-only-domain\",\"filesystem\":\"apfs\","
           "\"map_accuracy\":\"exact-bounded-spaceman\",\"unit_size\":%u,"
           "\"total_units\":%" PRIu64 ",\"cell_count\":%" PRIu64
           ",\"total_bytes\":%" PRIu64 ",\"cells\":[",
           analysis->block_size, analysis->block_count, cells,
           analysis->block_count * (uint64_t)analysis->block_size);
    for (uint64_t index = 0U; index < cells; ++index) {
        const uint64_t start = index * analysis->block_count / cells;
        uint64_t end = (index + 1U) * analysis->block_count / cells;
        if (end <= start)
            end = start + 1U;
        const uint64_t used = overlap(
            analysis->used_ranges, analysis->used_range_count, start, end);
        const uint64_t fragmented = overlap(
            analysis->fragmented_ranges, analysis->fragmented_range_count,
            start, end);
        const uint64_t free_count = (end - start) - used;
        used_total += used;
        free_total += free_count;
        if (index != 0U)
            putchar(',');
        printf("{\"start\":%" PRIu64 ",\"end\":%" PRIu64
               ",\"free\":%" PRIu64 ",\"used\":%" PRIu64
               ",\"unknown\":0,\"bad\":0,\"fragmented\":%" PRIu64
               ",\"directory\":0,\"outside\":0}",
               start, end - 1U, free_count, used, fragmented);
    }
    const double percent = infiltratr_percent_u64(
        analysis->fragmented_files, analysis->regular_files);
    printf("],\"free_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64
           ",\"unknown_bytes\":0,\"details\":{\"container_uuid\":\"",
           free_total * analysis->block_size, used_total * analysis->block_size);
    print_uuid(analysis->container_uuid);
    printf("\",\"xid\":%" PRIu64 ",\"active_nx_block\":%" PRIu64
           ",\"spaceman_block\":%" PRIu64
           ",\"fragmentation_available\":true,"
           "\"fragmentation_basis\":\"native APFS catalog file-extent records\","
           "\"allocation_basis\":\"active-checkpoint spaceman chunk bitmaps\"},"
           "\"regular_files\":%" PRIu64 ",\"directories\":%" PRIu64
           ",\"fragmented_files\":%" PRIu64
           ",\"fragmented_directories\":0,\"fragmentation_percent\":%.6f}\n",
           analysis->xid, analysis->active_nx_block, analysis->spaceman_block,
           analysis->regular_files, analysis->directories,
           analysis->fragmented_files, percent);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "identify") == 0) {
        ApfsSummary summary;
        char error[256] = {0};
        if (apfs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        printf("{\"filesystem\":\"apfs\",\"block_size\":%u,\"block_count\":%" PRIu64 "}\n",
               summary.block_size, summary.block_count);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "analyse-json") == 0) {
        ApfsAnalysis analysis;
        char error[512] = {0};
        if (apfs_analyse(argv[2], &analysis, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        print_analysis(&analysis);
        apfs_analysis_free(&analysis);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (!infiltratr_parse_u64_range(argv[4], 10U, 1U, UINT64_MAX, &cells)) {
            fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        ApfsAnalysis analysis;
        char error[512] = {0};
        if (apfs_analyse(argv[2], &analysis, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        const int rc = print_map(&analysis, cells);
        apfs_analysis_free(&analysis);
        return rc;
    }
    usage(stderr);
    return 2;
}
