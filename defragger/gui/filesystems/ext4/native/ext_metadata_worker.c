// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_NAME "linux-defragger-ext-metadata-worker"

#ifndef LINUX_S_IFMT
#define LINUX_S_IFMT 0170000
#endif
#ifndef LINUX_S_IFDIR
#define LINUX_S_IFDIR 0040000
#endif
#ifndef LINUX_S_IFREG
#define LINUX_S_IFREG 0100000
#endif

typedef struct {
    ExtRangeVec ranges;
    bool in_run;
    uint64_t start;
    uint64_t previous;
} PayloadRanges;

static void usage(FILE *stream)
{
    (void)fprintf(stream, "Usage: %s analyse-json DEVICE\n", PROGRAM_NAME);
}

static void flush_payload_run(PayloadRanges *payload)
{
    if (!payload->in_run) return;
    ext_range_push(&payload->ranges, payload->start, payload->previous + 1U);
    payload->in_run = false;
}

static int collect_payload_block(ExtFs *fs, uint32_t ino, int64_t logical,
                                 uint64_t *physical, bool mutable_mapping,
                                 void *private_data, char **error)
{
    (void)fs;
    (void)ino;
    (void)logical;
    (void)mutable_mapping;
    (void)error;
    PayloadRanges *payload = private_data;
    if (!payload->in_run) {
        payload->start = *physical;
        payload->previous = *physical;
        payload->in_run = true;
    } else if (*physical == payload->previous + 1U) {
        payload->previous = *physical;
    } else {
        flush_payload_run(payload);
        payload->start = *physical;
        payload->previous = *physical;
        payload->in_run = true;
    }
    return 0;
}

static int collect_user_inode(ExtFs *fs, ExtInode *inode,
                              void *private_data, char **error)
{
    if (inode->mode == 0U || inode->links == 0U) return 0;
    unsigned kind = (unsigned)inode->mode & LINUX_S_IFMT;
    if (kind != LINUX_S_IFREG && kind != LINUX_S_IFDIR) return 0;
    if (inode->number < ext_fs_first_inode(fs) &&
        inode->number != EXT_ROOT_INO)
        return 0;

    PayloadRanges *payload = private_data;
    flush_payload_run(payload);
    if (ext_fs_iterate_payload(fs, inode, false, collect_payload_block,
                               payload, error) != 0)
        return -1;
    flush_payload_run(payload);
    return 0;
}

static bool payload_contains(const ExtRangeVec *ranges, size_t *index,
                             uint64_t block)
{
    while (*index < ranges->count && ranges->items[*index].end <= block)
        (*index)++;
    return *index < ranges->count &&
           ranges->items[*index].start <= block &&
           block < ranges->items[*index].end;
}

static int emit_metadata_ranges(ExtFs *fs, char **error)
{
    PayloadRanges payload = {0};
    if (ext_fs_foreach_inode(fs, collect_user_inode, &payload, error) != 0) {
        ext_range_free(&payload.ranges);
        return -1;
    }
    flush_payload_run(&payload);
    ext_range_sort_merge(&payload.ranges);

    bool first = true;
    bool in_run = false;
    uint64_t run_start = 0U;
    size_t payload_index = 0U;
    uint64_t total_blocks = ext_fs_blocks_count(fs);
    uint64_t first_data = ext_fs_first_data_block(fs);

    (void)putchar('[');
    for (uint64_t block = 0U; block < total_blocks; ++block) {
        bool allocated = block < first_data;
        if (!allocated &&
            ext_fs_block_allocated(fs, block, &allocated, error) != 0) {
            ext_range_free(&payload.ranges);
            return -1;
        }
        bool marked = allocated &&
            !payload_contains(&payload.ranges, &payload_index, block);
        if (marked && !in_run) {
            run_start = block;
            in_run = true;
        }
        if (in_run && (!marked || block + 1U == total_blocks)) {
            uint64_t run_end = marked ? block + 1U : block;
            if (!first) (void)putchar(',');
            first = false;
            (void)printf("[%" PRIu64 ",%" PRIu64 "]", run_start, run_end);
            in_run = false;
        }
    }
    (void)putchar(']');
    ext_range_free(&payload.ranges);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "analyse-json") != 0) {
        usage(stderr);
        return 2;
    }

    char *error = NULL;
    ExtFs *fs = NULL;
    if (ext_fs_open(argv[2], false, &fs, &error) != 0 ||
        ext_fs_validate_metadata(fs, false, &error) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROGRAM_NAME,
                      error != NULL ? error : "cannot open EXT filesystem");
        free(error);
        ext_fs_close(fs);
        return 1;
    }

    (void)printf("{\"block_size\":%u,\"total_blocks\":%" PRIu64
                 ",\"metadata_ranges\":",
                 ext_fs_block_size(fs), ext_fs_blocks_count(fs));
    if (emit_metadata_ranges(fs, &error) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROGRAM_NAME,
                      error != NULL ? error : "cannot classify EXT metadata");
        free(error);
        ext_fs_close(fs);
        return 1;
    }
    (void)puts("}");
    ext_fs_close(fs);
    return 0;
}
