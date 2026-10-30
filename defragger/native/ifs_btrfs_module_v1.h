/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_BTRFS_MODULE_V1_H
#define IFS_BTRFS_MODULE_V1_H
#include "ifs_defrag_services_v1.h"
typedef struct IfsBtrfsFixtureHostV1 IfsBtrfsFixtureHostV1;
#define IFS_BTRFS_MODULE_V1 1u
#define IFS_BTRFS_CAP_RUN UINT64_C(1)
#define IFS_BTRFS_CAP_FIXTURE UINT64_C(2)
typedef struct {
 uint32_t abi_version;
 uint32_t structure_bytes;
 const char *format_id;
 uint64_t capabilities;
 int (*run)(int,char **,const IfsDefragServicesV1 *);
 int (*fixture)(const void *,const char *,int,const IfsDefragServicesV1 *,const IfsBtrfsFixtureHostV1 *);
} IfsBtrfsModuleV1;
const IfsBtrfsModuleV1 *infiltrator_filesystem_btrfs_v1(void);
#endif
