// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEFRAGMENTER_TEST_MEDIA_RAW_H
#define DEFRAGMENTER_TEST_MEDIA_RAW_H

#include "test_media.h"
#include <stdbool.h>

/* The deterministic corpus is filesystem-neutral. Production creation and
 * inspection delegate exclusively to trusted Common V2 providers. Expected
 * file identities and lengths never come from mutable saved state. */
#define LDTM_RAW_FILES 16U
typedef struct {
    const LdtmFilesystemSpec *spec;
    uint32_t seen;
    uint64_t done[LDTM_RAW_FILES];
} LdtmRawCheck;

uint64_t ldtm_raw_file_bytes(const LdtmFilesystemSpec *spec, unsigned file);
void ldtm_raw_name(unsigned file, char output[16]);
int ldtm_raw_name_index(const char *name);
void ldtm_raw_payload(uint8_t *, size_t, unsigned, uint64_t);
void ldtm_raw_check_init(LdtmRawCheck *check, const LdtmFilesystemSpec *spec);
int ldtm_raw_check_file(LdtmRawCheck *check, unsigned file, uint64_t bytes);
int ldtm_raw_check_finish(const LdtmRawCheck *check);

/* Common V2 is the only accepted Test Media filesystem interface. */
int ldtm_common_v2_available(const LdtmFilesystemSpec *);
int ldtm_common_v2_media_available(const LdtmFilesystemSpec *);
int ldtm_common_v2_media_detail(const LdtmFilesystemSpec *, char *, size_t);
int ldtm_common_v2_verify_fragmentation(const LdtmFilesystemSpec *, const char *, unsigned *);
int ldtm_common_v2_fixture(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_raw_fat(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_fat_common_available(const LdtmFilesystemSpec *);
int ldtm_fat_common_create(const LdtmFilesystemSpec *, const char *);
int ldtm_fat_common_verify(const LdtmFilesystemSpec *, const char *);
int ldtm_fat_module_available(const LdtmFilesystemSpec *);
int ldtm_fat_module_create(const LdtmFilesystemSpec *, const char *);
int ldtm_raw_exfat(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_ext_module_available(const LdtmFilesystemSpec *);
int ldtm_raw_ext(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_minix_module_available(const LdtmFilesystemSpec *);
int ldtm_minix_module_fixture(const LdtmFilesystemSpec *,const char *,bool);
int ldtm_raw_minix(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_swap_module_available(const LdtmFilesystemSpec *);
int ldtm_raw_swap(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_hfsplus_module_available(const LdtmFilesystemSpec *);
int ldtm_hfsplus_module_fixture(const LdtmFilesystemSpec *,const char *,bool);
int ldtm_hfs_module_available(const LdtmFilesystemSpec *);
int ldtm_hfs_module_fixture(const LdtmFilesystemSpec *,const char *,bool);
int ldtm_raw_mac(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_ntfs_module_available(const LdtmFilesystemSpec *);
int ldtm_raw_ntfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_xfs_module_available(const LdtmFilesystemSpec *);
int ldtm_raw_xfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_btrfs_module_available(const LdtmFilesystemSpec *);
int ldtm_btrfs_module_fixture(const LdtmFilesystemSpec *,const char *,bool);
int ldtm_raw_btrfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_zfs_module_available(const LdtmFilesystemSpec *);
int ldtm_raw_zfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_raw_fixture(const LdtmFilesystemSpec *, const char *, bool, char *, size_t);

#endif
