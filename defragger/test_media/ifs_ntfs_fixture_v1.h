/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host-owned, format-neutral test-corpus functions. Versioned and size
 * validated before any disk I/O. Dataset layouts have a matching header.
 */
#ifndef IFS_NTFS_FIXTURE_V1_H
#define IFS_NTFS_FIXTURE_V1_H
#include "test_media_raw.h"
#define IFS_NTFS_FIXTURE_V1 1u
typedef struct IfsNtfsFixtureHostV1 {
    uint32_t abi_version;
    uint32_t structure_bytes;
    uint64_t (*file_bytes)(const LdtmFilesystemSpec *, unsigned);
    void (*name)(unsigned, char output[16]);
    int (*open)(const char *, bool, uint64_t *);
    int (*read)(int, uint64_t, void *, size_t);
    int (*write)(int, uint64_t, const void *, size_t);
    int (*plan)(const LdtmFilesystemSpec *, uint64_t, uint32_t, uint64_t, LdtmRawPlan *);
    int (*write_payload)(int, const LdtmRawPlan *);
    void (*check_init)(LdtmRawCheck *, const LdtmFilesystemSpec *);
    int (*check_file)(LdtmRawCheck *, unsigned, uint64_t);
    int (*check_data)(LdtmRawCheck *, int, unsigned, uint64_t, uint64_t, uint64_t);
    int (*check_finish)(const LdtmRawCheck *);
} IfsNtfsFixtureHostV1;
#endif
