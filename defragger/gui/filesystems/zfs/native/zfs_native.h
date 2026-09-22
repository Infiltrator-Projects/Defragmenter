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
    LdZfsByteOrder byte_order;
} LdZfsSummary;

int zfs_read_summary(const char *path, LdZfsSummary *summary,
                     char *error, size_t error_size);
bool zfs_probe(const char *path);
const char *zfs_byte_order_name(const LdZfsSummary *summary);

#endif
