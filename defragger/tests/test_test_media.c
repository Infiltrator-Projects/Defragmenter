// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "test-media check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int test_mmc_cid_identity_is_independent_of_udev(void)
{
    const char cid_lower[] = "1b534d303030303010ffffffff123456";
    const char cid_upper[] = "1B534D303030303010FFFFFFFF123456";
    char first[256];
    char second[256];
    char changed_slot[256];
    char changed_size[256];
    char short_buffer[8];

    /* A confirmed MMC identity is built from immutable sysfs card CID,
     * actual kernel device number and capacity, NOT udev-derived serial,
     * WWN, model or transport. This reproduces those properties arriving
     * during desktop unmount without weakening card replacement checks. */
    CHECK(ldtm_mmc_identity_material(
        UINT64_C(127865454592), 179U, 0U, cid_lower,
        first, sizeof(first)) == 0);
    CHECK(ldtm_mmc_identity_material(
        UINT64_C(127865454592), 179U, 0U, cid_upper,
        second, sizeof(second)) == 0);
    CHECK(strcmp(first, second) == 0);
    CHECK(strstr(first, "mmc-cid-v1") != NULL);
    CHECK(strstr(first, "rdev=179:0") != NULL);
    CHECK(ldtm_mmc_identity_material(
        UINT64_C(127865454592), 179U, 1U, cid_lower,
        changed_slot, sizeof(changed_slot)) == 0);
    CHECK(strcmp(first, changed_slot) != 0);
    CHECK(ldtm_mmc_identity_material(
        UINT64_C(127865454593), 179U, 0U, cid_lower,
        changed_size, sizeof(changed_size)) == 0);
    CHECK(strcmp(first, changed_size) != 0);
    CHECK(ldtm_mmc_identity_material(1U, 179U, 0U,
        "1b534d303030303010ffffffff123457",
        changed_slot, sizeof(changed_slot)) == 0);
    CHECK(strcmp(first, changed_slot) != 0);
    CHECK(ldtm_mmc_identity_material(
        UINT64_C(127865454592), 179U, 0U,
        "00000000000000000000000000000000", second, sizeof(second)) != 0);
    CHECK(ldtm_mmc_identity_material(
        UINT64_C(127865454592), 179U, 0U,
        "invalid-CID", second, sizeof(second)) != 0);
    CHECK(ldtm_mmc_identity_material(
        UINT64_C(127865454592), 179U, 0U, cid_lower,
        short_buffer, sizeof(short_buffer)) != 0);
    CHECK(ldtm_mmc_identity_material(
        0U, 179U, 0U, cid_lower, second, sizeof(second)) != 0);
    return 0;
}

int main(void) {
    char script[8192];
    const LdtmFilesystemSpec *fat12 = ldtm_find_spec("fat12");
    const LdtmFilesystemSpec *fat16 = ldtm_find_spec("fat16");
    const LdtmFilesystemSpec *ofs = ldtm_find_spec("ofs");
    const LdtmFilesystemSpec *ffs = ldtm_find_spec("ffs");
    const LdtmFilesystemSpec *sfs = ldtm_find_spec("sfs");
    const LdtmFilesystemSpec *pfs3 = ldtm_find_spec("pfs3");
    const LdtmFilesystemSpec *xfs = ldtm_find_spec("xfs");
    const LdtmFilesystemSpec *btrfs = ldtm_find_spec("btrfs");
    const LdtmFilesystemSpec *ufs = ldtm_find_spec("ufs2");
    const LdtmFilesystemSpec *zfs = ldtm_find_spec("zfs");
    const LdtmFilesystemSpec *apfs = ldtm_find_spec("apfs");
    LdtmFragmentProfile small;
    LdtmFragmentProfile normal;
    LdtmFragmentProfile sfs_profile;
    LdtmFragmentProfile pfs3_profile;
    LdtmFragmentProfile xfs_profile;
    LdtmFragmentProfile btrfs_profile;
    LdtmFragmentProfile zfs_profile;
    size_t count = 0U;
    const char *cursor;

    CHECK(ldtm_spec_count() == 44U);
    CHECK(ldtm_allocated_capacity_bytes() == UINT64_C(88319) * LDTM_MIB);
    CHECK(ldtm_required_capacity_bytes() == (UINT64_C(88319) * LDTM_MIB) + LDTM_GIB);
    CHECK(fat12 != NULL && fat16 != NULL && ofs != NULL && ffs != NULL && sfs != NULL && pfs3 != NULL);
    CHECK(xfs != NULL && btrfs != NULL && ufs != NULL && zfs != NULL && apfs != NULL);

    small = ldtm_fragment_profile(fat12);
    normal = ldtm_fragment_profile(fat16);
    sfs_profile = ldtm_fragment_profile(sfs);
    pfs3_profile = ldtm_fragment_profile(pfs3);
    xfs_profile = ldtm_fragment_profile(xfs);
    btrfs_profile = ldtm_fragment_profile(btrfs);
    zfs_profile = ldtm_fragment_profile(zfs);
    CHECK(ldtm_target_payload_bytes(fat12) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(fat16) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(ofs) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(ffs) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(sfs) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(pfs3) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_fragment_profile(ofs).directory_initial == 0U &&
          ldtm_fragment_profile(ffs).directory_second == 0U);
    CHECK(small.directory_initial == 0U && small.directory_second == 0U);
    CHECK(normal.files == LDTM_TARGET_FILE_COUNT && normal.chunks == 30U &&
          normal.chunk_kib == 2048U && normal.directory_initial == 0U &&
          normal.directory_second == 0U);
    CHECK(ldtm_profile_file_chunks(&normal, 0U) == 2U &&
          ldtm_profile_file_chunks(&normal, 7U) == 30U &&
          ldtm_profile_file_bytes(&normal, 0U) == UINT64_C(4) * LDTM_MIB &&
          ldtm_profile_file_bytes(&normal, 7U) == UINT64_C(60) * LDTM_MIB &&
          ldtm_profile_payload_bytes(&normal) == UINT64_C(200) * LDTM_MIB);
    CHECK(sfs_profile.files == LDTM_TARGET_FILE_COUNT &&
          sfs_profile.chunks == 30U && sfs_profile.chunk_kib == 2048U);
    CHECK(sfs_profile.directory_initial == 0U && sfs_profile.directory_second == 0U);
    CHECK(pfs3_profile.files == LDTM_TARGET_FILE_COUNT &&
          pfs3_profile.chunks == 30U && pfs3_profile.chunk_kib == 2048U);
    CHECK(pfs3_profile.directory_initial == 0U && pfs3_profile.directory_second == 0U);
    CHECK(xfs_profile.files == LDTM_TARGET_FILE_COUNT &&
          xfs_profile.directory_initial == 0U &&
          xfs_profile.directory_second == 0U &&
          ldtm_profile_payload_bytes(&xfs_profile) == UINT64_C(200) * LDTM_MIB);
    CHECK(btrfs_profile.files == LDTM_TARGET_FILE_COUNT &&
          btrfs_profile.directory_initial == 0U &&
          btrfs_profile.directory_second == 0U &&
          ldtm_profile_payload_bytes(&btrfs_profile) == UINT64_C(200) * LDTM_MIB);
    CHECK(zfs_profile.files == LDTM_TARGET_FILE_COUNT &&
          zfs_profile.directory_initial == 0U &&
          zfs_profile.directory_second == 0U &&
          ldtm_profile_payload_bytes(&zfs_profile) == UINT64_C(200) * LDTM_MIB);

    CHECK(ofs->creator == LDTM_CREATOR_GENERIC_V2);
    CHECK(ffs->creator == LDTM_CREATOR_GENERIC_V2);
    CHECK(sfs->creator == LDTM_CREATOR_GENERIC_V2);
    CHECK(strcmp(ldtm_creator_display_name(fat12), "Filesystem driver") == 0);
    CHECK(strcmp(ldtm_creator_display_name(ofs), "Filesystem driver") == 0);
    CHECK(strcmp(ldtm_creator_display_name(ffs), "Filesystem driver") == 0);
    CHECK(strcmp(ldtm_creator_display_name(sfs), "Filesystem driver") == 0);
    CHECK(strcmp(ldtm_creator_display_name(pfs3), "Filesystem driver") == 0);
    CHECK(strcmp(ldtm_creator_display_name(apfs), "Filesystem driver") == 0);
    CHECK(strcmp(ldtm_creator_display_name(NULL), "Unavailable") == 0);
    CHECK(strstr(sfs->note, "SFS0") != NULL &&
          strstr(sfs->note, "200 MiB heterogeneous") != NULL);
    CHECK(pfs3->creator == LDTM_CREATOR_GENERIC_V2 &&
          strstr(pfs3->note, "200 MiB heterogeneous") != NULL);
    CHECK(strstr(ufs->note, "UFS2") != NULL &&
          strstr(ufs->note, "exact fragmentation") != NULL);
    CHECK(strstr(zfs->note, "Common V2") != NULL &&
          strstr(zfs->note, "analysis-only") != NULL);
    CHECK(fat12->size_mib == 255U);
    CHECK(fat16->size_mib == 2048U);
    CHECK(ofs->size_mib == 2048U && ffs->size_mib == 2048U);
    CHECK(sfs->size_mib == 2048U && pfs3->size_mib == 2048U);
    CHECK(zfs->size_mib == 2048U && apfs->size_mib == 2048U);
    CHECK(apfs->creator == LDTM_CREATOR_GENERIC_V2);
    CHECK(ldtm_build_sfdisk_script(script, sizeof(script)) == 0);
    CHECK(strstr(script, "label: gpt") == script);
    CHECK(strstr(script, "name=\"LD_FAT16\"") != NULL);
    CHECK(strstr(script, "name=\"LD_FFS\"") != NULL);
    CHECK(ldtm_is_reserved_partition_label("LD_SFS") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_PFS3") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_APFS") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_OFS") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_HFSPLUS") == 0);

    /*
     * Transport is descriptive metadata only. A selected Test Media disk is
     * admitted or rejected by the whole-disk, system-use, writable, capacity,
     * stable-identity and explicit destructive-confirmation checks around this
     * policy point, so fixed, virtual and unclassified transports are valid.
     */
    CHECK(test_mmc_cid_identity_is_independent_of_udev() == 0);
    CHECK(ldtm_transport_is_field_media(0, "mmc") == 1);
    CHECK(ldtm_transport_is_field_media(0, "usb") == 1);
    CHECK(ldtm_transport_is_field_media(0, "sd") == 1);
    CHECK(ldtm_transport_is_field_media(1, "unknown") == 1);
    CHECK(ldtm_transport_is_field_media(0, "nvme") == 1);
    CHECK(ldtm_transport_is_field_media(0, "sata") == 1);
    CHECK(ldtm_transport_is_field_media(0, "sas") == 1);
    CHECK(ldtm_transport_is_field_media(0, "virtio") == 1);
    CHECK(ldtm_transport_is_field_media(0, "") == 1);
    CHECK(ldtm_transport_is_field_media(0, NULL) == 1);

    CHECK(ldtm_build_sfdisk_script(script, sizeof(script)) == 0);
    CHECK(strstr(script, "label: gpt") != NULL);
    CHECK(strstr(script, "size=255MiB, type=linux, name=\"LD_FAT12\"") != NULL);
    CHECK(strstr(script, "size=2048MiB, type=linux, name=\"LD_SFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_FAT12\"") != NULL);
    CHECK(strstr(script, "name=\"LD_OFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_FFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_PFS3\"") != NULL);
    CHECK(strstr(script, "name=\"LD_UFS2\"") != NULL);
    CHECK(strstr(script, "name=\"LD_UFS1\"") != NULL);
    CHECK(strstr(script, "name=\"LD_SFS2\"") != NULL);
    CHECK(ldtm_find_spec("ufs1") != NULL && ldtm_find_spec("sfs2") != NULL);
    CHECK(ldtm_find_spec("ufs") == NULL);
    CHECK(strstr(script, "name=\"LD_ZFS\"") != NULL);
    CHECK(strstr(script, "type=linux, name=\"LD_SWAP\"") != NULL);
    cursor = script;
    while ((cursor = strstr(cursor, "name=\"LD_")) != NULL) { ++count; cursor += 6; }
    CHECK(count == ldtm_spec_count());
    /* Every new profile appears exactly once and uses only the generic
     * Common V2 corpus path. */
    const char *const added[] = {"adfs", "bcachefs", "befs", "bfs", "cpm", "efs", "f2fs", "fosfat", "gfs2", "hpfs", "jfs", "nilfs2", "ocfs2", "omfs", "qnx4", "qnx6", "reiserfs", "sysv", "udf", "vmfs", "vmfs6"};
    for (size_t i = 0U; i < sizeof(added)/sizeof(added[0]); ++i) {
        const LdtmFilesystemSpec *spec = ldtm_find_spec(added[i]);
        CHECK(spec && spec->creator == LDTM_CREATOR_GENERIC_V2);
        CHECK(spec->size_mib == 2048U && ldtm_target_payload_bytes(spec) == UINT64_C(200)*LDTM_MIB);
        CHECK(strstr(script, spec->label) != NULL);
    }

    puts("test-media core tests passed");
    return 0;
}
