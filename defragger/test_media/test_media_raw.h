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
int ldtm_common_v2_media_available(const LdtmFilesystemSpec *);
int ldtm_common_v2_media_detail(const LdtmFilesystemSpec *, char *, size_t);
/* BFS modes: 0 compact, 1 genuine free-space holes, 2 growth gaps. */
int ldtm_common_v2_verify_bfs_layout(const LdtmFilesystemSpec *, const char *, int);
int ldtm_common_v2_verify_fragmentation(const LdtmFilesystemSpec *, const char *, unsigned *);
int ldtm_common_v2_verify_growth(const LdtmFilesystemSpec *, const char *);
int ldtm_common_v2_fixture(const LdtmFilesystemSpec *, const char *, bool);
int ldtm_raw_fixture(const LdtmFilesystemSpec *, const char *, bool, char *, size_t);

#endif
