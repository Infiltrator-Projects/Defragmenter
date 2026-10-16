// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_NAME "defragmenter-ext-metadata-worker"

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
    uint32_t total_inodes;
    uint32_t last_progress_inode;
} PayloadRanges;

typedef struct {
    ExtRangeVec ranges;
} AllocatedRanges;

static void usage(FILE *stream)
{
    (void)fprintf(stream, "Usage: %s analyse-json DEVICE\n", PROGRAM_NAME);
}

static void report_progress(PayloadRanges *payload, uint32_t inode)
{
    if (payload->total_inodes == 0U)
        return;
    if (inode < payload->last_progress_inode + 4096U &&
        inode != payload->total_inodes)
        return;
    payload->last_progress_inode = inode;
    (void)fprintf(stderr,
        "@@ANALYSIS {\"phase\":\"Classifying EXT metadata\","
        "\"completed\":%u,\"total\":%u}\n",
        inode, payload->total_inodes);
    (void)fflush(stderr);
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
    PayloadRanges *payload = private_data;
    report_progress(payload, inode->number);
    if (inode->mode == 0U || inode->links == 0U) return 0;
    unsigned kind = (unsigned)inode->mode & LINUX_S_IFMT;
    if (kind != LINUX_S_IFREG && kind != LINUX_S_IFDIR) return 0;
    if (inode->number < ext_fs_first_inode(fs) &&
        inode->number != EXT_ROOT_INO)
        return 0;

    flush_payload_run(payload);
    if (ext_fs_iterate_payload(fs, inode, false, collect_payload_block,
                               payload, error) != 0)
        return -1;
    flush_payload_run(payload);
    return 0;
}

static int collect_allocated_run(uint64_t start, uint64_t length,
                                 bool allocated, void *private_data,
                                 char **error)
{
    (void)error;
    if (allocated && length != 0U) {
        AllocatedRanges *ranges = private_data;
        ext_range_push(&ranges->ranges, start, start + length);
    }
    return 0;
}

static void emit_range(bool *first, uint64_t start, uint64_t end)
{
    if (end <= start) return;
    if (!*first) (void)putchar(',');
    *first = false;
    (void)printf("[%" PRIu64 ",%" PRIu64 "]", start, end);
}

static int emit_metadata_ranges(ExtFs *fs, char **error)
{
    PayloadRanges payload = {
        .total_inodes = ext_fs_inodes_count(fs),
    };
    if (ext_fs_foreach_inode(fs, collect_user_inode, &payload, error) != 0) {
        ext_range_free(&payload.ranges);
        return -1;
    }
    flush_payload_run(&payload);
    report_progress(&payload, payload.total_inodes);
    ext_range_sort_merge(&payload.ranges);

    AllocatedRanges allocated = {0};
    const uint64_t first_data = ext_fs_first_data_block(fs);
    if (first_data != 0U)
        ext_range_push(&allocated.ranges, 0U, first_data);
    if (ext_fs_foreach_block_run(fs, collect_allocated_run,
                                 &allocated, error) != 0) {
        ext_range_free(&payload.ranges);
        ext_range_free(&allocated.ranges);
        return -1;
    }
    ext_range_sort_merge(&allocated.ranges);

    bool first = true;
    size_t payload_index = 0U;
    (void)putchar('[');
    for (size_t allocated_index = 0U;
         allocated_index < allocated.ranges.count; ++allocated_index) {
        const ExtRange span = allocated.ranges.items[allocated_index];
        uint64_t cursor = span.start;
        while (payload_index < payload.ranges.count &&
               payload.ranges.items[payload_index].end <= cursor)
            payload_index++;
        size_t check = payload_index;
        while (check < payload.ranges.count &&
               payload.ranges.items[check].start < span.end) {
            const ExtRange used = payload.ranges.items[check];
            if (used.start > cursor)
                emit_range(&first, cursor,
                           used.start < span.end ? used.start : span.end);
            if (used.end > cursor) cursor = used.end;
            if (cursor >= span.end) break;
            check++;
        }
        if (cursor < span.end)
            emit_range(&first, cursor, span.end);
    }
    (void)putchar(']');
    ext_range_free(&payload.ranges);
    ext_range_free(&allocated.ranges);
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
