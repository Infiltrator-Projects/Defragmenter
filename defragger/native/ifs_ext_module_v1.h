/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_EXT_MODULE_V1_H
#define IFS_EXT_MODULE_V1_H
#include "ifs_defrag_services_v1.h"
typedef struct IfsExtFixtureHostV1 IfsExtFixtureHostV1;
#define IFS_EXT_MODULE_V1 1u
#define IFS_EXT_CAP_RUN UINT64_C(1)
#define IFS_EXT_CAP_METADATA UINT64_C(2)
#define IFS_EXT_CAP_FIXTURE UINT64_C(4)
#define IFS_EXT_CAP_PROBE UINT64_C(8)
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*probe)(const char *, const IfsDefragServicesV1 *);
    int (*run)(int, char **, const IfsDefragServicesV1 *);
    int (*metadata)(int, char **, const IfsDefragServicesV1 *);
    int (*fixture)(const void *, const char *, int, const IfsDefragServicesV1 *, const IfsExtFixtureHostV1 *);
} IfsExtModuleV1;
const IfsExtModuleV1 *infiltrator_filesystem_ext_v1(void);
#endif
