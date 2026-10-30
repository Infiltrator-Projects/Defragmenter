/* SPDX-License-Identifier: GPL-3.0-or-later
 * SWAP.so's format-specific host command and disposable-media ABI.
 * The executable host supplies validated I/O; the driver owns all layout.
 */
#ifndef INFILTRATOR_SWAP_MODULE_V1_H
#define INFILTRATOR_SWAP_MODULE_V1_H
#include <stdint.h>
#define IFS_SWAP_MODULE_V1 1u
#define IFS_SWAP_CAP_RUN UINT64_C(1)
#define IFS_SWAP_CAP_FORMAT UINT64_C(2)
#define IFS_SWAP_CAP_VERIFY UINT64_C(4)
typedef struct {
    void *context;
    uint64_t bytes;
    int (*read_at)(void *, uint64_t, void *, uint32_t);
    int (*write_at)(void *, uint64_t, const void *, uint32_t);
    int (*flush)(void *);
} IfsSwapIoV1;
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*run)(int, char **);
    int (*format)(const IfsSwapIoV1 *, const char *);
    int (*verify)(const IfsSwapIoV1 *);
} IfsSwapModuleV1;
/* Use dlsym() for this entry and require exact version, size and format. */
const IfsSwapModuleV1 *infiltrator_filesystem_swap_v1(void);
#endif
