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
            ",\"size_bytes\":%llu,\"label_index\":%u,"
            "\"uberblock_slot\":%u,\"uberblock_magic_offset\":%llu,"
            "\"uberblock_version\":%llu,\"uberblock_txg\":%llu,"
            "\"uberblock_guid_sum\":%llu,\"uberblock_timestamp\":%llu,"
            "\"candidate_uberblocks\":%u,"
            "\"root_vdev\":%llu,\"root_offset\":%llu,"
            "\"root_asize\":%llu,\"root_lsize\":%llu,\"root_psize\":%llu,"
            "\"root_logical_birth\":%llu,\"root_compression\":%u,"
            "\"root_checksum\":%u,\"root_type\":%u,\"root_level\":%u,"
            "\"root_embedded\":%s",
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
            summary->root_embedded ? "true" : "false");
    }
    (void)puts("}");
}

static void print_map_json(const LdZfsSummary *summary, uint64_t requested_cells)
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
                 "\"unknown_bytes\":%llu,\"cells\":[",
                 ZFS_MAP_UNIT_SIZE,
                 (unsigned long long)total_units,
                 (unsigned long long)cell_count,
                 (unsigned long long)rounded_bytes,
                 (unsigned long long)rounded_bytes);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = (index * total_units) / cell_count;
        const uint64_t end_exclusive = ((index + 1U) * total_units) / cell_count;
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
        "\"label_basis\":\"four OpenZFS leaf-vdev labels and 128-entry uberblock rings\","
        "\"note\":\"Committed ZFS label/uberblock parsed natively; exact allocation still requires MOS, metaslab and space-map traversal\"}}\n",
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
        summary->root_embedded ? "true" : "false");
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
        char error[256];
        if (zfs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, error);
            return 1;
        }
        print_map_json(&summary, requested_cells);
        return 0;
    }

    if (argc != 3) {
        usage(stderr);
        return 2;
    }

    LdZfsSummary summary;
    char error[256];
    if (zfs_read_summary(argv[2], &summary, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return 1;
    }

    if (strcmp(argv[1], "identify") == 0) {
        print_summary_json(&summary, 0);
        return 0;
    }
    if (strcmp(argv[1], "analyse-json") == 0) {
        print_summary_json(&summary, 1);
        return 0;
    }

    usage(stderr);
    return 2;
}
