// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_TEST_MEDIA_H
#define LINUX_DEFRAGGER_TEST_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#define LDTM_MIB UINT64_C(1048576)
#define LDTM_GIB UINT64_C(1073741824)
#define LDTM_SPEC_COUNT 21U
#define LDTM_TARGET_FILE_COUNT 8U
#define LDTM_STATE_ROOT "/var/lib/linux-defragger-test-media"

typedef enum {
    LDTM_CREATOR_FAT12,
    LDTM_CREATOR_FAT16,
    LDTM_CREATOR_FAT32,
    LDTM_CREATOR_EXFAT,
    LDTM_CREATOR_NTFS,
    LDTM_CREATOR_EXT2,
    LDTM_CREATOR_EXT3,
    LDTM_CREATOR_EXT4,
    LDTM_CREATOR_XFS,
    LDTM_CREATOR_BTRFS,
    LDTM_CREATOR_AFFS,
    LDTM_CREATOR_PFS3,
    LDTM_CREATOR_HFS,
    LDTM_CREATOR_HFSPLUS,
    LDTM_CREATOR_MINIX,
    LDTM_CREATOR_UFS,
    LDTM_CREATOR_ZFS,
    LDTM_CREATOR_APFS,
    LDTM_CREATOR_MANUAL,
    LDTM_CREATOR_SWAP
} LdtmCreator;

typedef struct {
    const char *key;
    const char *label;
    uint32_t size_mib;
    uint32_t payload_mib;
    LdtmCreator creator;
    const char *package_hint;
    const char *note;
} LdtmFilesystemSpec;

typedef struct {
    uint32_t anchors;
    uint32_t anchor_kib;
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
const char *ldtm_creator_program(const LdtmFilesystemSpec *spec);
const char *ldtm_creator_display_name(const LdtmFilesystemSpec *spec);
int ldtm_resolve_program(const char *program, char *output, size_t output_capacity);
int ldtm_program_available(const char *program);
int ldtm_spec_creator_available(const LdtmFilesystemSpec *spec,
                                char *detail, size_t detail_capacity);
int ldtm_format_amiga_volume(const char *path, uint8_t dostype, const char *label);
int ldtm_validate_amiga_volume(const char *path, uint8_t expected_dostype);
int ldtm_populate_amiga_volume(const char *path, uint8_t dostype,
                               const LdtmFragmentProfile *profile);
int ldtm_verify_amiga_payload(const char *path, uint8_t dostype,
                              const LdtmFragmentProfile *profile,
                              char *detail, size_t detail_capacity);
int ldtm_verify_amiga_payload_after_defrag(
    const char *path, uint8_t dostype,
    const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity);
int ldtm_format_apfs_volume(const char *path);
int ldtm_verify_apfs_payload(const char *path,
                             char *detail, size_t detail_capacity);
int ldtm_verify_apfs_payload_after_defrag(
    const char *path, char *detail, size_t detail_capacity);
int ldtm_canonicalize_device(const char *input, char *output, size_t output_capacity);
int ldtm_is_whole_block_device(const char *device);
int ldtm_is_system_disk(const char *device);
int ldtm_device_safety_check(const char *device, int allow_non_removable,
                             char *detail, size_t detail_capacity);
int ldtm_device_fingerprint(const char *device, char output[65]);
int ldtm_is_reserved_partition_label(const char *label);
int ldtm_sanitize_reserved_partitions(const char *device);
int ldtm_worker_prepare(const char *device, const char *confirmed_device,
                        const char *confirmed_fingerprint);
int ldtm_worker_verify(const char *device);
void ldtm_apply_mb_theme(void);
int ldtm_gui_main(int argc, char **argv);

#endif
