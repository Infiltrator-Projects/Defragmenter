// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_PFS3_NATIVE_H
#define LD_PFS3_NATIVE_H

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
    uint32_t options;
    uint32_t disk_type;
    uint32_t reserved_block_size;
} Pfs3Analysis;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t free_count;
    uint64_t used_count;
    uint64_t fragmented_count;
    uint64_t outside_count;
} Pfs3MapCell;

int pfs3_analyse(const char *path, Pfs3Analysis *analysis,
                 Pfs3MapCell *cells, uint64_t cell_count,
                 char *error, size_t error_size);
bool pfs3_probe(const char *path);
int pfs3_build_stage(const char *source, const char *stage, bool growth,
                     unsigned growth_percent, bool live_updates,
                     uint64_t *commit_bytes, char *error, size_t error_size);
int pfs3_verify_layout(const char *path, bool growth, unsigned growth_percent,
                       char *error, size_t error_size);
int pfs3_commit_stage(const char *stage, const char *target, uint64_t *written,
                      char *error, size_t error_size);

#endif
