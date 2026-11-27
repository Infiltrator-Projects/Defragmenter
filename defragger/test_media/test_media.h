// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEFRAGMENTER_TEST_MEDIA_H
#define DEFRAGMENTER_TEST_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#define LDTM_MIB UINT64_C(1048576)
#define LDTM_GIB UINT64_C(1073741824)
#define LDTM_SPEC_LIMIT 128U /* standard GPT entry capacity, not a format list */
#define LDTM_TARGET_FILE_COUNT 8U
#define LDTM_STATE_ROOT "/var/lib/defragmenter-test-media"

typedef enum {
    LDTM_CREATOR_GENERIC_V2,
    LDTM_CREATOR_NO_NAMESPACE,
    LDTM_CREATOR_MANUAL /* installed provider has no usable format profile */
} LdtmCreator;

typedef struct {
    const char *key;
    const char *label;
    uint32_t size_mib;
    uint32_t payload_mib;
    LdtmCreator creator;
    const char *note;
} LdtmFilesystemSpec;

typedef struct {
    uint32_t files;
    /* Maximum chunks in any target. Individual targets may be smaller. */
    uint32_t chunks;
    uint32_t chunk_kib;
    uint32_t directory_initial;
    uint32_t directory_second;
    /* Zero means use chunks, preserving compact unit-test profiles. */
    uint32_t file_chunks[LDTM_TARGET_FILE_COUNT];
} LdtmFragmentProfile;

const LdtmFilesystemSpec *ldtm_specs(void);
size_t ldtm_spec_count(void);
int ldtm_catalogue_fingerprint(char output[65]);
const LdtmFilesystemSpec *ldtm_find_spec(const char *key);
uint64_t ldtm_required_capacity_bytes(void);
uint64_t ldtm_allocated_capacity_bytes(void);
LdtmFragmentProfile ldtm_fragment_profile(const LdtmFilesystemSpec *spec);
uint32_t ldtm_profile_file_chunks(const LdtmFragmentProfile *profile, size_t file_index);
uint64_t ldtm_profile_file_bytes(const LdtmFragmentProfile *profile, size_t file_index);
uint64_t ldtm_profile_payload_bytes(const LdtmFragmentProfile *profile);
uint64_t ldtm_target_payload_bytes(const LdtmFilesystemSpec *spec);
int ldtm_build_sfdisk_script(char *buffer, size_t capacity);
int ldtm_transport_is_field_media(int removable, const char *transport);
int ldtm_decode_hex_byte(char high, char low, unsigned char *value);
const char *ldtm_creator_display_name(const LdtmFilesystemSpec *spec);
int ldtm_required_modules_available(char *detail, size_t detail_capacity);
int ldtm_resolve_program(const char *program, char *output, size_t output_capacity);
int ldtm_program_available(const char *program);
int ldtm_spec_creator_available(const LdtmFilesystemSpec *spec,
                                char *detail, size_t detail_capacity);
/* Physical media layout policy: GPT creation currently requires 512-byte logical sectors. */
#define LDTM_MEDIA_SECTOR_BYTES 512u
int ldtm_canonicalize_device(const char *input, char *output, size_t output_capacity);
int ldtm_is_whole_block_device(const char *device);
int ldtm_is_system_disk(const char *device);
int ldtm_device_safety_check(const char *device, int allow_non_removable,
                             char *detail, size_t detail_capacity);
int ldtm_device_fingerprint(const char *device, char output[65]);
/* Pure hardware-CID identity material; rejects missing/invalid CID and overflow. */
int ldtm_mmc_identity_material(
    uint64_t size_bytes, unsigned int major_number, unsigned int minor_number,
    const char *cid, char *material, size_t capacity);
int ldtm_is_reserved_partition_label(const char *label);
int ldtm_worker_prepare(const char *device, const char *confirmed_device,
                        const char *confirmed_fingerprint, const char *confirmed_catalogue);
int ldtm_worker_verify(const char *device);
int ldtm_worker_qualify(const char *device);
void ldtm_apply_mb_theme(void);
int ldtm_gui_main(int argc, char **argv);

#endif
