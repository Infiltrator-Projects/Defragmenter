/* SPDX-License-Identifier: GPL-3.0-or-later
 * FAT12/FAT16 shared full-lifecycle userspace interface backed by ChaN FatFs.
 * V1 compatibility APIs remain distinct; source is compiled into EACH .so.
 */
#ifndef IFS_FATFS_MODULE_H
#define IFS_FATFS_MODULE_H
#include <stdint.h>
#define IFS_FATFS_MODULE_ABI 3u
#define IFS_FATFS_CAP_READ UINT64_C(1)
#define IFS_FATFS_CAP_WRITE_EXISTING UINT64_C(2)
#define IFS_FATFS_CAP_CREATE UINT64_C(4)
#define IFS_FATFS_CAP_DELETE UINT64_C(8)
#define IFS_FATFS_CAP_RENAME UINT64_C(16)
#define IFS_FATFS_CAP_DIRECTORY UINT64_C(32)
#define IFS_FATFS_CAP_TRUNCATE UINT64_C(64)
typedef struct {
    void *user;
    uint64_t partition_bytes;
    uint32_t sector_bytes;
    int (*read_at)(void *, uint64_t, void *, uint32_t);
    int (*write_at)(void *, uint64_t, const void *, uint32_t);
    int (*flush)(void *);
    uint32_t (*fat_timestamp)(void *);
} IfsFatfsIo;
typedef struct {
    char name[768]; /* UTF-8 LFN name buffer (255 Unicode code units max) */
    uint32_t bytes;
    uint8_t attributes;
} IfsFatfsEntry;
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*mount)(const IfsFatfsIo *);
    int (*unmount)(void);
    int (*create)(const char *);
    int (*read)(const char *, uint32_t, void *, uint32_t);
    int (*write)(const char *, uint32_t, const void *, uint32_t);
    int (*truncate)(const char *, uint32_t);
    int (*mkdir)(const char *);
    int (*unlink)(const char *);
    int (*rename)(const char *, const char *);
    int (*stat)(const char *, IfsFatfsEntry *);
    int (*directory_entry)(const char *, uint32_t, IfsFatfsEntry *);
} IfsFatfsModuleV3;
const IfsFatfsModuleV3 *infiltrator_filesystem_module_v3(void);

/* V4 extends V3 without changing its layout. Formatting is destructive and
 * operates only on the partition described by the caller's I/O callbacks.
 * cluster_bytes is zero for automatic selection or a power-of-two allocation
 * unit in bytes. A dry-run checks the resulting FAT width before any write. */
#define IFS_FATFS_MODULE_ABI_V4 4u
#define IFS_FATFS_CAP_FORMAT UINT64_C(128)
#define IFS_FATFS_CAP_ATTRIBUTES UINT64_C(256)
#define IFS_FATFS_CAP_MODIFIED_TIME UINT64_C(512)
#define IFS_FATFS_CAP_SPACE UINT64_C(1024)
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    const IfsFatfsModuleV3 *v3;
    int (*format)(const IfsFatfsIo *, uint32_t cluster_bytes);
    int (*set_attributes)(const char *, uint8_t attributes, uint8_t mask);
    int (*set_modified_time)(const char *, uint16_t fat_date, uint16_t fat_time);
    int (*get_modified_time)(const char *, uint16_t *fat_date, uint16_t *fat_time);
    int (*space)(uint64_t *total_bytes, uint64_t *free_bytes);
} IfsFatfsModuleV4;
const IfsFatfsModuleV4 *infiltrator_filesystem_module_v4(void);
#endif
