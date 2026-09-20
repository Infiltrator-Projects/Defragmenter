// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_BTRFS_NATIVE_H
#define LINUX_DEFRAGGER_BTRFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t start;
    uint64_t end;
} BtrfsRange;

typedef struct {
    uint64_t total_bytes;
    uint64_t physical_bytes;
    uint64_t logical_bytes_used;
    uint64_t generation;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint32_t sector_size;
    uint32_t node_size;
    uint64_t device_id;
    size_t chunk_count;
    size_t chunk_tree_blocks;
    size_t root_tree_blocks;
    size_t extent_tree_blocks;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    uint64_t fragmented_directories;
    uint64_t filesystem_roots_scanned;
    uint64_t filesystem_tree_blocks;
    uint64_t malformed_items;
    BtrfsRange *used_ranges;
    size_t used_range_count;
    BtrfsRange *fragmented_ranges;
    size_t fragmented_range_count;
    bool fragmentation_available;
} BtrfsAnalysis;

bool btrfs_probe(const char *path);
int btrfs_analyse(const char *path, BtrfsAnalysis *analysis,
                  char *error, size_t error_size);
void btrfs_analysis_free(BtrfsAnalysis *analysis);
int btrfs_build_stage(const char *source, const char *stage, bool growth,
                      unsigned growth_percent, bool live_updates,
                      uint64_t *commit_bytes, char *error, size_t error_size);
int btrfs_verify_layout(const char *path, bool growth, unsigned growth_percent,
                        char *error, size_t error_size);

#endif
