/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_APFS_MODULE_V1_H
#define IFS_APFS_MODULE_V1_H
#include "ifs_defrag_services_v1.h"
#define IFS_APFS_MODULE_V1 1u
#define IFS_APFS_CAP_RUN UINT64_C(1)
#define IFS_APFS_CAP_FIXTURE UINT64_C(2)
#define IFS_APFS_FIXTURE_FORMAT 1u
#define IFS_APFS_FIXTURE_POPULATE 2u
#define IFS_APFS_FIXTURE_VERIFY 3u
#define IFS_APFS_FIXTURE_VERIFY_DEFRAG 4u
typedef struct {
 uint32_t abi_version;
 uint32_t structure_bytes;
 const char *format_id;
 uint64_t capabilities;
 int (*run)(int argc, char **argv, const IfsDefragServicesV1 *services);
 int (*fixture)(uint32_t op, const char *path, const void *profile, char *detail,
                size_t capacity, const IfsDefragServicesV1 *services,
                const void *host_corpus);
} IfsApfsModuleV1;
const IfsApfsModuleV1 *infiltrator_filesystem_apfs_v1(void);
#endif
