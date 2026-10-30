// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEFRAGMENTER_TEST_MEDIA_RAW_H
#define DEFRAGMENTER_TEST_MEDIA_RAW_H

#include "test_media.h"
#include <stdbool.h>

/* Bounded qualification fixtures, not general-purpose filesystem formatters.
 * FAT12/16 creation delegates to installed modules; their codecs only verify.
 * Other transitional codecs own their records; this layer owns the corpus, I/O
 * and complete byte-verification contract. No expected-file list is supplied
 * by mutable saved state. File identities and lengths come from this corpus. */
#define LDTM_RAW_FILES 16U
typedef struct {
    uint64_t bytes;
    uint64_t start[2];
    uint64_t count[2];
} LdtmRawFile;
typedef struct {
    uint64_t capacity;
    uint32_t unit;
    LdtmRawFile files[LDTM_RAW_FILES];
    uint64_t end;
} LdtmRawPlan;
typedef struct {
    const LdtmFilesystemSpec *spec;
    uint32_t seen;
    uint64_t done[LDTM_RAW_FILES];
} LdtmRawCheck;

uint64_t ldtm_raw_file_bytes(const LdtmFilesystemSpec *spec, unsigned file);
void ldtm_raw_name(unsigned file, char output[16]);
int ldtm_raw_name_index(const char *name);
int ldtm_raw_open(const char *path, bool write, uint64_t *capacity);
int ldtm_raw_read(int fd, uint64_t offset, void *data, size_t bytes);
int ldtm_raw_write(int fd, uint64_t offset, const void *data, size_t bytes);
int ldtm_raw_plan(const LdtmFilesystemSpec *spec, uint64_t capacity, uint32_t unit, uint64_t first,
                  LdtmRawPlan *plan);
int ldtm_raw_write_payload(int fd, const LdtmRawPlan *plan);
void ldtm_raw_payload(uint8_t *, size_t, unsigned, uint64_t);
void ldtm_raw_check_init(LdtmRawCheck *check, const LdtmFilesystemSpec *spec);
int ldtm_raw_check_file(LdtmRawCheck *check, unsigned file, uint64_t bytes);
int ldtm_raw_check_data(LdtmRawCheck *check, int fd, unsigned file, uint64_t physical,
                        uint64_t logical, uint64_t bytes);
int ldtm_raw_check_finish(const LdtmRawCheck *check);
uint32_t ldtm_raw_crc32c(const void *data, size_t bytes);

int ldtm_raw_fat(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_fat_module_available(const LdtmFilesystemSpec *);
int ldtm_fat_module_create(const LdtmFilesystemSpec *, const char *);
int ldtm_raw_exfat(const LdtmFilesystemSpec *, const char *, bool);
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
int ldtm_raw_ntfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_raw_xfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_btrfs_module_available(const LdtmFilesystemSpec *);
int ldtm_btrfs_module_fixture(const LdtmFilesystemSpec *,const char *,bool);
int ldtm_raw_btrfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_zfs_module_available(const LdtmFilesystemSpec *);
int ldtm_raw_zfs(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_raw_fixture(const LdtmFilesystemSpec *, const char *, bool, char *, size_t);

#endif
