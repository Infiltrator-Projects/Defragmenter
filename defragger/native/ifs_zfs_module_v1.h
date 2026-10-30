/* SPDX-License-Identifier: GPL-3.0-or-later
 * ZFS.so versioned read-only analyser interface. Never advertises defrag,
 * disk mutations or generic filesystem creation capabilities.
 */
#ifndef IFS_ZFS_MODULE_V1_H
#define IFS_ZFS_MODULE_V1_H
#include <stdint.h>
#define IFS_ZFS_MODULE_V1 1u
#define IFS_ZFS_CAP_RUN UINT64_C(1)
#define IFS_ZFS_CAP_FIXTURE UINT64_C(2)
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*run)(int, char **);
    /* Portable test-media ABI: caller supplies only format-neutral corpus I/O.
     * The on-disk ZFS format and complete verifier live inside this module. */
    int (*fixture)(const void *spec, const char *path, int create, const void *host);
} IfsZfsModuleV1;
const IfsZfsModuleV1 *infiltrator_filesystem_zfs_v1(void);
#endif
