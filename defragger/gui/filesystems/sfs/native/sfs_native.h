// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_SFS_NATIVE_H
#define LD_SFS_NATIVE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_base;
    uint32_t bitmap_blocks;
    uint32_t root_object_container;
    uint32_t admin_space_container;
    uint32_t extent_bnode_root;
    uint32_t object_node_root;
    uint16_t structure_version;
    uint16_t sequence_number;
    uint8_t root_bits;
    uint64_t filesystem_bytes;
    uint64_t physical_bytes;
    uint64_t free_blocks;
    uint64_t used_blocks;
    uint64_t data_blocks;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    bool growth_10_satisfied;
    bool primary_root_valid;
    bool backup_root_valid;
    bool transaction_pending;
} SfsAnalysis;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t free_count;
    uint64_t used_count;
    uint64_t fragmented_count;
    uint64_t outside_count;
} SfsMapCell;

int sfs_analyse(const char *path, SfsAnalysis *analysis,
                SfsMapCell *cells, uint64_t cell_count,
                char *error, size_t error_size);
bool sfs_probe(const char *path);
int sfs_build_stage(const char *source, const char *stage, bool growth,
                    unsigned growth_percent, bool live_updates,
                    uint64_t *commit_bytes, char *error, size_t error_size);
int sfs_verify_layout(const char *path, bool growth, unsigned growth_percent,
                      char *error, size_t error_size);
int sfs_commit_stage(const char *stage, const char *target,
                     const char *target_identity, uint64_t target_bytes,
                     uint64_t *written, char *error, size_t error_size);
#endif
