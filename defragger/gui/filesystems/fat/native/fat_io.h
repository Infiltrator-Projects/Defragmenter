// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Buffered FAT cluster relocation pipeline.
 *
 * The caller owns policy and counters through FatIoConfig.  This module owns
 * extent coalescing, parallel source reads and ordered destination writes.
 */

#ifndef DEFRAGMENTER_FAT_IO_H
#define DEFRAGMENTER_FAT_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat_volume.h"

typedef struct {
    size_t ram_limit;
    size_t workers;
    bool rotational;
    bool serial_flash;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t read_extents;
    uint64_t write_extents;
} FatIoConfig;

void fat_io_copy_clusters(
    Fat32 *fs,
    const uint32_t *sources,
    const uint32_t *destinations,
    size_t count,
    FatIoConfig *config
);

#endif
