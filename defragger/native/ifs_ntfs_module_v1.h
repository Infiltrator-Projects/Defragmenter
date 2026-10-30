/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_NTFS_MODULE_V1_H
#define IFS_NTFS_MODULE_V1_H
#include <stdint.h>
#include "ifs_defrag_services_v1.h"
#include "ifs_ntfs_fixture_v1.h"
#define IFS_NTFS_MODULE_V1 1u
#define IFS_NTFS_CAP_RUN UINT64_C(1)
#define IFS_NTFS_CAP_FIXTURE UINT64_C(2)
typedef struct {
 uint32_t abi_version;
 uint32_t structure_bytes;
 const char *format_id;
 uint64_t capabilities;
 int (*run)(int argc, char **argv, const IfsDefragServicesV1 *services);
 int (*fixture)(const void *spec, const char *path, int create,
    const IfsDefragServicesV1 *services, const IfsNtfsFixtureHostV1 *host);
} IfsNtfsModuleV1;
const IfsNtfsModuleV1 *infiltrator_filesystem_ntfs_v1(void);
#endif
