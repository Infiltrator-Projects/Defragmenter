/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent FFS.so native defragmentation and recovery entrypoint.
 * The app provides only format-neutral safety, I/O and cancellation.
 */
#ifndef IFS_FFS_DEFRAG_V1_H
#define IFS_FFS_DEFRAG_V1_H
#include "ifs_defrag_services_v1.h"
#define IFS_FFS_DEFRAG_ABI_V1 1u
#define IFS_FFS_DEFRAG_CAP_RUN UINT64_C(1)
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*run)(int argc, char **argv, const IfsDefragServicesV1 *host);
} IfsFfsDefragV1;
const IfsFfsDefragV1 *infiltrator_filesystem_ffs_defrag_v1(void);
#endif
