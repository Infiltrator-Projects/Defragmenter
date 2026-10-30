/* SPDX-License-Identifier: GPL-3.0-or-later
 * FFS.so owns all FFS Test Media formatting, allocation and payload writing.
 */
#ifndef IFS_FFS_FIXTURE_V1_H
#define IFS_FFS_FIXTURE_V1_H
#include <stdint.h>
#include <stddef.h>
#include "ifs_defrag_services_v1.h"
#define IFS_FFS_FIXTURE_ABI_V1 1U
#define IFS_FFS_FIXTURE_CAP_FORMAT UINT64_C(1)
#define IFS_FFS_FIXTURE_CAP_POPULATE UINT64_C(2)
#define IFS_FFS_FIXTURE_CAP_VERIFY UINT64_C(4)
typedef struct {
    uint32_t files, chunks, chunk_kib, directory_initial, directory_second;
    uint32_t file_chunks[8];
} IfsFfsFragmentProfile;
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*format)(const char *, const char *, const IfsDefragServicesV1 *);
    int (*validate)(const char *, const IfsDefragServicesV1 *);
    int (*populate)(const char *, const IfsFfsFragmentProfile *, const IfsDefragServicesV1 *);
    int (*verify)(const char *, const IfsFfsFragmentProfile *, char *, size_t, const IfsDefragServicesV1 *);
    int (*verify_after)(const char *, const IfsFfsFragmentProfile *, char *, size_t, const IfsDefragServicesV1 *);
} IfsFfsFixtureV1;
const IfsFfsFixtureV1 *infiltrator_filesystem_ffs_fixture_v1(void);
#endif
