/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_HFSPLUS_MODULE_V1_H
#define IFS_HFSPLUS_MODULE_V1_H
#include "ifs_defrag_services_v1.h"
#define IFS_HFSPLUS_MODULE_V1 1u
#define IFS_HFSPLUS_CAP_RUN UINT64_C(1)
typedef struct {
 uint32_t abi_version;
 uint32_t structure_bytes;
 const char *format_id;
 uint64_t capabilities;
 int (*run)(int argc, char **argv, const IfsDefragServicesV1 *services);
} IfsHfsplusModuleV1;
const IfsHfsplusModuleV1 *infiltrator_filesystem_hfsplus_v1(void);
#endif
