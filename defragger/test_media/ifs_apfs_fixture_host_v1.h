/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_APFS_FIXTURE_HOST_V1_H
#define IFS_APFS_FIXTURE_HOST_V1_H
#include "test_media.h"
#define IFS_APFS_FIXTURE_HOST_V1 1u
typedef struct {
 uint32_t abi_version;
 uint32_t structure_bytes;
 const LdtmFilesystemSpec *(*find_spec)(const char *);
 LdtmFragmentProfile (*fragment_profile)(const LdtmFilesystemSpec *);
 uint32_t (*profile_file_chunks)(const LdtmFragmentProfile *, size_t);
 uint64_t (*profile_file_bytes)(const LdtmFragmentProfile *, size_t);
 uint64_t (*profile_payload_bytes)(const LdtmFragmentProfile *);
} IfsApfsFixtureHostV1;
#endif
