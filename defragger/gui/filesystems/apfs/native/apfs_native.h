// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_APFS_NATIVE_H
#define LD_APFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t start;
    uint64_t end;
} ApfsRange;

typedef struct {
    uint32_t block_size;
    uint64_t block_count;
    uint8_t container_uuid[16];
} ApfsSummary;

typedef struct {
    uint32_t block_size;
    uint64_t block_count;
    uint8_t container_uuid[16];
    uint64_t xid;
    uint64_t active_nx_block;
    uint64_t spaceman_block;
    uint64_t volume_oid;
    uint64_t volume_super_block;
    uint64_t free_blocks;
    uint64_t used_blocks;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    ApfsRange *used_ranges;
    size_t used_range_count;
    ApfsRange *fragmented_ranges;
    size_t fragmented_range_count;
} ApfsAnalysis;

int apfs_read_summary(const char *path, ApfsSummary *summary,
                      char *error, size_t error_size);
int apfs_analyse(const char *path, ApfsAnalysis *analysis,
                 char *error, size_t error_size);
void apfs_analysis_free(ApfsAnalysis *analysis);
int apfs_build_stage(const char *source, const char *stage,
                     bool growth, unsigned growth_percent,
                     bool live_updates, uint64_t *commit_bytes,
                     char *error, size_t error_size);
int apfs_verify_layout(const char *path, bool growth,
                       unsigned growth_percent,
                       char *error, size_t error_size);
bool apfs_probe(const char *path);

#endif
