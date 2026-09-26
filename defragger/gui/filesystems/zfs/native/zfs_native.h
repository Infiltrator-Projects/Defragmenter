// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_ZFS_NATIVE_H
#define LD_ZFS_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LD_ZFS_BYTE_ORDER_LITTLE = 1,
    LD_ZFS_BYTE_ORDER_BIG = 2,
} LdZfsByteOrder;

typedef struct {
    uint64_t size_bytes;
    uint64_t uberblock_magic_offset;
    uint64_t uberblock_txg;
    uint64_t uberblock_version;
    uint64_t uberblock_guid_sum;
    uint64_t uberblock_timestamp;
    uint32_t label_index;
    uint32_t uberblock_slot;
    uint32_t candidate_uberblocks;
    uint64_t root_vdev;
    uint64_t root_offset;
    uint64_t root_asize;
    uint64_t root_lsize;
    uint64_t root_psize;
    uint64_t root_logical_birth;
    uint32_t root_compression;
    uint32_t root_checksum;
    uint32_t root_type;
    uint32_t root_level;
    bool root_embedded;
    bool config_known;
    bool single_leaf_supported;
    bool mos_features_present;
    bool mos_features_supported;
    uint32_t mos_feature_count;
    char unsupported_mos_feature[64];
    uint64_t pool_guid;
    uint64_t leaf_guid;
    uint64_t top_guid;
    uint64_t top_vdev_id;
    uint64_t ashift;
    uint64_t metaslab_array;
    uint64_t metaslab_shift;
    uint64_t top_vdev_asize;
    uint32_t metaslab_vdevs;
    char top_vdev_type[16];
    LdZfsByteOrder byte_order;
} LdZfsSummary;

enum {
    LD_ZFS_RANGE_ALLOCATED = 1U << 0,
    LD_ZFS_RANGE_FRAGMENTED = 1U << 1,
    LD_ZFS_RANGE_RESERVED = 1U << 2,
};

typedef struct {
    uint64_t start;
    uint64_t length;
    uint32_t flags;
} LdZfsRange;

typedef struct {
    uint64_t size_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t unknown_bytes;
    uint64_t fragmented_bytes;
    uint64_t files_seen;
    uint64_t fragmented_files;
    uint64_t allocated_extents;
    bool exact_allocation;
    bool exact_fragmentation;
    LdZfsRange *ranges;
    size_t range_count;
    size_t range_capacity;
} LdZfsAnalysis;

int zfs_read_summary(const char *path, LdZfsSummary *summary,
                     char *error, size_t error_size);
int zfs_analyse_exact(const char *path, LdZfsAnalysis *analysis,
                      char *error, size_t error_size);
void zfs_analysis_destroy(LdZfsAnalysis *analysis);
bool zfs_probe(const char *path);
const char *zfs_byte_order_name(const LdZfsSummary *summary);

#endif
