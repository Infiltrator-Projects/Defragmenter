/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_UFS_PROBE_V1_H
#define IFS_UFS_PROBE_V1_H
#include <stdint.h>
#define IFS_UFS_PROBE_ABI_V1 1u
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    /* Read-only format probe. 0: accepted; nonzero: not this on-disk format. */
    int (*accepts_volume)(const char *path);
} IfsUfsProbeV1;
const IfsUfsProbeV1 *infiltrator_filesystem_ufs_probe_v1(void);
#endif
