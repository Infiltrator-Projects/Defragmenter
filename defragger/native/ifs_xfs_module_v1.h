/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_XFS_MODULE_V1_H
#define IFS_XFS_MODULE_V1_H
#include "ifs_defrag_services_v1.h"
#define IFS_XFS_MODULE_V1 1u
#define IFS_XFS_CAP_RUN UINT64_C(1)
typedef struct {
 uint32_t abi_version;
 uint32_t structure_bytes;
 const char *format_id;
 uint64_t capabilities;
 int (*run)(int, char **, const IfsDefragServicesV1 *);
} IfsXfsModuleV1;
const IfsXfsModuleV1 *infiltrator_filesystem_xfs_v1(void);
#endif
