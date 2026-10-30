/* SPDX-License-Identifier: GPL-3.0-or-later
 * exFAT's 64-bit file API. Independent freestanding shared object.
 * The V3/V4 operations remain available for existing 32-bit callers.
 */
#ifndef IFS_EXFAT_SO_API_H
#define IFS_EXFAT_SO_API_H
#include <stdint.h>
#include "fat_module_abi.h"

#define IFS_EXFAT_ABI_VERSION_V5 5u
#define IFS_EXFAT_CAP_READ64 UINT64_C(1)
#define IFS_EXFAT_CAP_WRITE64 UINT64_C(2)
#define IFS_EXFAT_CAP_TRUNCATE64 UINT64_C(4)
#define IFS_EXFAT_CAP_STAT64 UINT64_C(8)
typedef struct {
    uint64_t size_bytes;
    uint8_t attributes;
    char name[768];
} IfsExfatEntry64;
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    const IfsFatfsModuleV4 *compat_v4;
    int (*read64)(const char *,uint64_t,void *,uint32_t);
    int (*write64)(const char *,uint64_t,const void *,uint32_t);
    int (*truncate64)(const char *,uint64_t);
    int (*stat64)(const char *,IfsExfatEntry64 *);
} IfsExfatModuleV5;
const IfsExfatModuleV5 *infiltrator_filesystem_module_v5(void);
#endif
