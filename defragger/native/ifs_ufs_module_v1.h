/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_UFS_MODULE_V1_H
#define IFS_UFS_MODULE_V1_H
#include "ifs_defrag_services_v1.h"
#define IFS_UFS_MODULE_V1 1u
#define IFS_UFS_CAP_RUN UINT64_C(1)
#define IFS_UFS_CAP_TEST_MEDIA UINT64_C(2)
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*run)(int argc, char **argv, const IfsDefragServicesV1 *services);
    int (*format_test_media)(int argc, char **argv);
} IfsUfsModuleV1;
const IfsUfsModuleV1 *infiltrator_filesystem_ufs_v1(void);
#endif
