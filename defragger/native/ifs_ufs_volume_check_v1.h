/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS-owned validation for media manufactured by UFS.so.
 * This is not an independent payload oracle.
 */
#ifndef IFS_UFS_VOLUME_CHECK_V1_H
#define IFS_UFS_VOLUME_CHECK_V1_H
#include <stdint.h>
#define IFS_UFS_VOLUME_CHECK_ABI_V1 1u
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    /* Return 0 only for a valid little-endian UFS2 filesystem. */
    int (*is_ufs2_little_endian)(const char *device);
} IfsUfsVolumeCheckV1;
const IfsUfsVolumeCheckV1 *infiltrator_filesystem_ufs_volume_check_v1(void);
#endif
