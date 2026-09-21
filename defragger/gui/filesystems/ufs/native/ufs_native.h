// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_UFS_NATIVE_H
#define LD_UFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LD_UFS_VARIANT_UFS1_LE = 1,
    LD_UFS_VARIANT_UFS1_BE = 2,
    LD_UFS_VARIANT_UFS2_LE = 3,
    LD_UFS_VARIANT_UFS2_BE = 4,
} LdUfsVariant;

typedef struct {
    LdUfsVariant variant;
    uint64_t superblock_offset;
    uint64_t magic_offset;
    bool allocation_totals_known;
    bool cylinder_geometry_known;
    uint32_t block_size;
    uint32_t fragment_size;
    uint32_t fragments_per_block;
    uint64_t filesystem_fragments;
    uint64_t data_fragments;
    uint64_t free_blocks;
    uint64_t free_fragments;
    uint32_t cylinder_groups;
    uint32_t cylinder_group_size;
    uint32_t fragments_per_group;
    uint32_t inodes_per_group;
    uint32_t cylinder_block_fragment;
    uint32_t inode_block_fragment;
    uint32_t first_data_fragment;
    uint32_t inodes_per_block;
    uint32_t indirects_per_block;
    int32_t old_cylinder_offset;
    int32_t old_cylinder_mask;
    uint8_t clean;
    uint32_t flags;
    uint32_t metadata_check_hashes;
    int32_t contiguous_summary_size;
    uint64_t pending_blocks;
    uint32_t pending_inodes;
    uint32_t snapshot_count;
} LdUfsSummary;

typedef struct {
    LdUfsSummary summary;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint64_t total_units;
    uint64_t free_fragments_exact;
    uint64_t used_fragments_exact;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    uint64_t fragmented_directories;
    uint64_t sparse_files;
    uint64_t indirect_files;
} LdUfsAnalysis;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t free_count;
    uint64_t used_count;
    uint64_t outside_count;
    uint64_t fragmented_count;
    uint64_t directory_count;
} LdUfsMapCell;

int ufs_read_summary(const char *path, LdUfsSummary *summary,
                     char *error, size_t error_size);
int ufs_analyse_allocation(const char *path, LdUfsAnalysis *analysis,
                           LdUfsMapCell *cells, uint64_t cell_count,
                           char *error, size_t error_size);
int ufs_build_stage(const char *source, const char *stage,
                    bool growth, unsigned growth_percent,
                    bool live_updates, uint64_t *commit_bytes,
                    char *error, size_t error_size);
int ufs_verify_layout(const char *path, bool growth,
                      unsigned growth_percent,
                      char *error, size_t error_size);
bool ufs_probe(const char *path);
const char *ufs_variant_name(const LdUfsSummary *summary);
const char *ufs_byte_order_name(const LdUfsSummary *summary);
unsigned int ufs_version(const LdUfsSummary *summary);
uint64_t ufs_recorded_data_bytes(const LdUfsSummary *summary);
uint64_t ufs_recorded_free_bytes(const LdUfsSummary *summary);
uint64_t ufs_recorded_used_bytes(const LdUfsSummary *summary);

#endif
