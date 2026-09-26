// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "affs_native.h"
#include "sfs_native.h"
#include "pfs3_native.h"
#include "apfs_native.h"

#include <fcntl.h>
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

static uint32_t test_load_be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void test_store_be32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value >> 24U);
    bytes[1] = (unsigned char)(value >> 16U);
    bytes[2] = (unsigned char)(value >> 8U);
    bytes[3] = (unsigned char)value;
}

static int production_parser_rejects(const char *path)
{
    AffsVolume volume;
    char *error = NULL;
    const int accepted = affs_scan(path, false, &volume, &error) == 0;
    if (accepted)
        affs_close(&volume);
    free(error);
    return accepted ? 1 : 0;
}

static int set_amiga_dostype(const char *path, uint8_t dostype)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const ssize_t written = pwrite(fd, &dostype, 1U, 3);
    const int sync_result = written == 1 ? fsync(fd) : -1;
    const int close_result = close(fd);
    return written == 1 && sync_result == 0 && close_result == 0 ? 0 : -1;
}

static int set_amiga_bitmap_valid(const char *path, uint32_t value)
{
    enum { AMIGA_BLOCK_BYTES = 512, AMIGA_LONGWORDS = 128,
           AMIGA_ROOT_BITMAP_VALID_WORD = 78, AMIGA_CHECKSUM_WORD = 5 };
    unsigned char root[AMIGA_BLOCK_BYTES];
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const off_t bytes = lseek(fd, 0, SEEK_END);
    if (bytes <= 0 || bytes % AMIGA_BLOCK_BYTES != 0) {
        (void)close(fd);
        return -1;
    }
    const uint64_t blocks = (uint64_t)bytes / AMIGA_BLOCK_BYTES;
    const uint64_t root_offset = (blocks / 2U) * AMIGA_BLOCK_BYTES;
    if (pread(fd, root, sizeof(root), (off_t)root_offset) != (ssize_t)sizeof(root)) {
        (void)close(fd);
        return -1;
    }

    test_store_be32(root + AMIGA_ROOT_BITMAP_VALID_WORD * 4U, value);
    test_store_be32(root + AMIGA_CHECKSUM_WORD * 4U, 0U);
    uint32_t sum = 0U;
    for (size_t word = 0U; word < AMIGA_LONGWORDS; ++word)
        sum += test_load_be32(root + word * 4U);
    test_store_be32(root + AMIGA_CHECKSUM_WORD * 4U, 0U - sum);

    const ssize_t written =
        pwrite(fd, root, sizeof(root), (off_t)root_offset);
    const int sync_result = written == (ssize_t)sizeof(root) ? fsync(fd) : -1;
    const int close_result = close(fd);
    return written == (ssize_t)sizeof(root) &&
           sync_result == 0 && close_result == 0 ? 0 : -1;
}

static int production_parser_accepts(const char *path, uint8_t dostype, int expect_ffs,
                                     size_t expected_files, size_t fragmented_targets,
                                     size_t minimum_fragments) {
    AffsVolume volume;
    char *error = NULL;
    size_t fragmented = 0U;
    int result = 1;
    if (affs_scan(path, false, &volume, &error) == 0) {
        size_t index;
        for (index = 0U; index < volume.files.n; ++index) {
            if (volume.files.v[index].byte_size > 0U &&
                affs_fragments(&volume.files.v[index].data) >= minimum_fragments) {
                ++fragmented;
            }
        }
        if (volume.dostype == dostype && (volume.ffs ? 1 : 0) == expect_ffs &&
            volume.bitmap_blocks.n > 25U && volume.bitmap_ext_blocks.n > 0U &&
            volume.files.n == expected_files && fragmented == fragmented_targets) result = 0;
        affs_close(&volume);
    }
    free(error);
    return result;
}

static int corrupt_first_payload_block(const char *path) {
    AffsVolume volume;
    char *error = NULL;
    uint32_t data_block = 0U;
    int fd = -1;
    unsigned char byte;
    size_t index;
    int result = 1;
    if (affs_scan(path, false, &volume, &error) != 0) goto cleanup;
    for (index = 0U; index < volume.files.n; ++index) {
        if (volume.files.v[index].byte_size > 0U && volume.files.v[index].data.n > 0U) {
            data_block = volume.files.v[index].data.v[0];
            break;
        }
    }
    affs_close(&volume);
    if (data_block == 0U) goto cleanup;
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) goto cleanup;
    if (pread(fd, &byte, 1U, (off_t)data_block * 512 + 64) != 1) goto cleanup;
    byte ^= UINT8_C(0x5a);
    if (pwrite(fd, &byte, 1U, (off_t)data_block * 512 + 64) != 1 || fsync(fd) != 0) goto cleanup;
    result = 0;
cleanup:
    if (fd >= 0) (void)close(fd);
    free(error);
    return result;
}

static int test_amiga_formatters_and_payload(void) {
    char path[] = "/tmp/linux-defragger-amiga-media.XXXXXX";
    const LdtmFragmentProfile tiny = {0U, 0U, 2U, 4U, 8U, 16U, 16U};
    const size_t retained_entries = tiny.directory_initial / 2U + tiny.directory_second;
    const size_t expected_files = (size_t)tiny.files + retained_entries;
    char detail[512];
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)(128U * LDTM_MIB)) != 0 || close(fd) != 0) {
        (void)unlink(path);
        return 1;
    }
    if (ldtm_format_amiga_volume(path, 0U, "LD_OFS") != 0 ||
        ldtm_validate_amiga_volume(path, 0U) != 0 ||
        ldtm_validate_amiga_volume(path, 1U) == 0 ||
        production_parser_accepts(path, 0U, 0, 0U, 0U, 1U) != 0 ||
        set_amiga_bitmap_valid(path, 0U) != 0 ||
        production_parser_rejects(path) != 0 ||
        set_amiga_bitmap_valid(path, UINT32_MAX) != 0 ||
        production_parser_accepts(path, 0U, 0, 0U, 0U, 1U) != 0 ||
        set_amiga_dostype(path, 6U) != 0 ||
        production_parser_rejects(path) != 0 ||
        set_amiga_dostype(path, 0U) != 0 ||
        production_parser_accepts(path, 0U, 0, 0U, 0U, 1U) != 0 ||
        ldtm_populate_amiga_volume(path, 0U, &tiny) != 0 ||
        ldtm_verify_amiga_payload(path, 0U, &tiny, detail, sizeof(detail)) != 0 ||
        production_parser_accepts(path, 0U, 0, expected_files, tiny.files, tiny.chunks) != 0 ||
        0) {
        (void)unlink(path);
        return 1;
    }
    {
        char stage[256];
        char *stage_error = NULL;
        uint64_t commit_bytes = 0U;
        if (snprintf(stage, sizeof(stage), "%s.ofs-stage", path) <= 0 ||
            affs_build_stage(path, stage, false, 10U, false,
                             &commit_bytes, &stage_error) != 0 ||
            ldtm_verify_amiga_payload_after_defrag(
                stage, 0U, &tiny, detail, sizeof(detail)) != 0) {
            free(stage_error);
            (void)unlink(stage);
            (void)unlink(path);
            return 1;
        }
        free(stage_error);
        if (unlink(stage) != 0) {
            (void)unlink(path);
            return 1;
        }
    }
    if (corrupt_first_payload_block(path) != 0 ||
        ldtm_verify_amiga_payload(path, 0U, &tiny, detail, sizeof(detail)) == 0) {
        (void)unlink(path);
        return 1;
    }
    if (ldtm_format_amiga_volume(path, 1U, "LD_FFS") != 0 ||
        ldtm_validate_amiga_volume(path, 1U) != 0 ||
        ldtm_validate_amiga_volume(path, 0U) == 0 ||
        production_parser_accepts(path, 1U, 1, 0U, 0U, 1U) != 0 ||
        ldtm_populate_amiga_volume(path, 1U, &tiny) != 0 ||
        ldtm_verify_amiga_payload(path, 1U, &tiny, detail, sizeof(detail)) != 0 ||
        production_parser_accepts(path, 1U, 1, expected_files, tiny.files, tiny.chunks) != 0 ||
        0) {
        (void)unlink(path);
        return 1;
    }
    {
        char stage[256];
        char *stage_error = NULL;
        uint64_t commit_bytes = 0U;
        if (snprintf(stage, sizeof(stage), "%s.ffs-stage", path) <= 0 ||
            affs_build_stage(path, stage, false, 10U, false,
                             &commit_bytes, &stage_error) != 0 ||
            ldtm_verify_amiga_payload_after_defrag(
                stage, 1U, &tiny, detail, sizeof(detail)) != 0) {
            free(stage_error);
            (void)unlink(stage);
            (void)unlink(path);
            return 1;
        }
        free(stage_error);
        if (unlink(stage) != 0) {
            (void)unlink(path);
            return 1;
        }
    }
    if (corrupt_first_payload_block(path) != 0 ||
        ldtm_verify_amiga_payload(path, 1U, &tiny, detail, sizeof(detail)) == 0) {
        (void)unlink(path);
        return 1;
    }
    return unlink(path) == 0 ? 0 : 1;
}

static int test_sfs_formatter_and_payload(void) {
    char path[] = "/tmp/linux-defragger-sfs-media.XXXXXX";
    char detail[512];
    char error[256] = {0};
    SfsAnalysis analysis;
    const LdtmFilesystemSpec *sfs = ldtm_find_spec("sfs");
    LdtmFragmentProfile profile;
    int fd;
    unsigned char byte;
    if (sfs == NULL) return 1;
    profile = ldtm_fragment_profile(sfs);
    fd = mkstemp(path);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)(64U * LDTM_MIB)) != 0 || close(fd) != 0) {
        (void)unlink(path);
        return 1;
    }
    if (ldtm_format_amiga_volume(path, 1U, "LD_SFS") != 0 ||
        ldtm_populate_amiga_volume(path, 1U, &profile) != 0 ||
        ldtm_verify_amiga_payload(path, 1U, &profile, detail, sizeof(detail)) != 0 ||
        sfs_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) != 0 ||
        analysis.regular_files != 1U || analysis.fragmented_files != 1U ||
        analysis.data_blocks != 6400U || analysis.growth_10_satisfied) {
        (void)unlink(path);
        return 1;
    }
    {
        char stage[256];
        uint64_t commit_bytes = 0U;
        if (snprintf(stage, sizeof(stage), "%s.stage", path) <= 0 ||
            sfs_build_stage(path, stage, false, 10U, false,
                            &commit_bytes, error, sizeof(error)) != 0 ||
            ldtm_verify_amiga_payload_after_defrag(
                stage, 1U, &profile, detail, sizeof(detail)) != 0) {
            (void)unlink(stage);
            (void)unlink(path);
            return 1;
        }
        if (unlink(stage) != 0) {
            (void)unlink(path);
            return 1;
        }
    }
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 || pread(fd, &byte, 1U, (off_t)128U * 4096U) != 1) {
        if (fd >= 0) (void)close(fd);
        (void)unlink(path);
        return 1;
    }
    byte ^= UINT8_C(0xa5);
    if (pwrite(fd, &byte, 1U, (off_t)128U * 4096U) != 1 || fsync(fd) != 0) {
        (void)close(fd);
        (void)unlink(path);
        return 1;
    }
    (void)close(fd);
    if (ldtm_verify_amiga_payload(path, 1U, &profile, detail, sizeof(detail)) == 0) {
        (void)unlink(path);
        return 1;
    }
    return unlink(path) == 0 ? 0 : 1;
}

static int test_pfs3_formatter_and_payload(void) {
    char path[] = "/tmp/linux-defragger-pfs3-media.XXXXXX";
    char detail[512];
    char error[256] = {0};
    Pfs3Analysis analysis;
    const LdtmFilesystemSpec *pfs3 = ldtm_find_spec("pfs3");
    LdtmFragmentProfile profile;
    int fd;
    unsigned char byte;
    if (pfs3 == NULL) return 1;
    profile = ldtm_fragment_profile(pfs3);
    fd = mkstemp(path);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)(128U * LDTM_MIB)) != 0 || close(fd) != 0) {
        (void)unlink(path);
        return 1;
    }
    if (ldtm_format_amiga_volume(path, 1U, "LD_PFS3") != 0 ||
        ldtm_populate_amiga_volume(path, 1U, &profile) != 0 ||
        ldtm_verify_amiga_payload(path, 1U, &profile, detail, sizeof(detail)) != 0 ||
        pfs3_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) != 0 ||
        analysis.regular_files != 1U || analysis.fragmented_files != 1U ||
        analysis.data_blocks != 51200U || analysis.growth_10_satisfied) {
        (void)unlink(path);
        return 1;
    }
    {
        char stage[256];
        uint64_t commit_bytes = 0U;
        if (snprintf(stage, sizeof(stage), "%s.stage", path) <= 0 ||
            pfs3_build_stage(path, stage, false, 10U, false,
                             &commit_bytes, error, sizeof(error)) != 0 ||
            ldtm_verify_amiga_payload_after_defrag(
                stage, 1U, &profile, detail, sizeof(detail)) != 0) {
            (void)unlink(stage);
            (void)unlink(path);
            return 1;
        }
        if (unlink(stage) != 0) {
            (void)unlink(path);
            return 1;
        }
    }
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 || pread(fd, &byte, 1U, (off_t)4096U * 512U) != 1) {
        if (fd >= 0) (void)close(fd);
        (void)unlink(path);
        return 1;
    }
    byte ^= UINT8_C(0x5a);
    if (pwrite(fd, &byte, 1U, (off_t)4096U * 512U) != 1 || fsync(fd) != 0) {
        (void)close(fd);
        (void)unlink(path);
        return 1;
    }
    (void)close(fd);
    if (ldtm_verify_amiga_payload(path, 1U, &profile, detail, sizeof(detail)) == 0) {
        (void)unlink(path);
        return 1;
    }
    return unlink(path) == 0 ? 0 : 1;
}


static int test_apfs_formatter_and_payload(void) {
    char path[] = "/tmp/linux-defragger-apfs-media.XXXXXX";
    char detail[512];
    char error[512] = {0};
    ApfsAnalysis analysis;
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)(128U * LDTM_MIB)) != 0 ||
        close(fd) != 0) {
        (void)unlink(path);
        return 1;
    }
    if (ldtm_format_apfs_volume(path) != 0 ||
        ldtm_verify_apfs_payload(
            path, detail, sizeof(detail)) != 0 ||
        apfs_analyse(path, &analysis,
                     error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    if (analysis.block_size != 4096U ||
        analysis.block_count != 32768U ||
        analysis.regular_files != 1U ||
        analysis.fragmented_files != 1U) {
        apfs_analysis_free(&analysis);
        (void)unlink(path);
        return 1;
    }
    apfs_analysis_free(&analysis);
    {
        char stage[256];
        uint64_t commit_bytes = 0U;
        if (snprintf(stage, sizeof(stage), "%s.stage", path) <= 0 ||
            apfs_build_stage(path, stage, false, 10U, false,
                             &commit_bytes, error, sizeof(error)) != 0 ||
            ldtm_verify_apfs_payload_after_defrag(
                stage, detail, sizeof(detail)) != 0) {
            (void)unlink(stage);
            (void)unlink(path);
            return 1;
        }
        if (unlink(stage) != 0) {
            (void)unlink(path);
            return 1;
        }
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        (void)unlink(path);
        return 1;
    }
    unsigned char byte;
    const off_t payload =
        (off_t)UINT64_C(512) * 4096;
    if (pread(fd, &byte, 1U, payload) != 1) {
        (void)close(fd);
        (void)unlink(path);
        return 1;
    }
    byte ^= UINT8_C(0x5a);
    if (pwrite(fd, &byte, 1U, payload) != 1 ||
        fsync(fd) != 0) {
        (void)close(fd);
        (void)unlink(path);
        return 1;
    }
    (void)close(fd);
    if (ldtm_verify_apfs_payload(
            path, detail, sizeof(detail)) == 0) {
        (void)unlink(path);
        return 1;
    }
    return unlink(path) == 0 ? 0 : 1;
}

static int reset_two_gib_sparse_image(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const int result =
        ftruncate(fd, 0) == 0 &&
        ftruncate(fd, (off_t)(UINT64_C(2) * LDTM_GIB)) == 0 ? 0 : -1;
    (void)close(fd);
    return result;
}

static int test_two_gib_first_party_media_geometry(void)
{
    char path[] = "/tmp/linux-defragger-2g-media.XXXXXX";
    char detail[512];
    int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    if (close(fd) != 0 || reset_two_gib_sparse_image(path) != 0) {
        (void)unlink(path);
        return 1;
    }

    /*
     * The old 1 GiB OFS/FFS and 64 MiB SFS limits were Test Media
     * constants, not format limits in our native creators.  Exercise each
     * creator on a fresh sparse 2 GiB image, matching the real worker's
     * wipefs-and-reformat contract rather than carrying bytes from the
     * previous filesystem into the next one.
     */
    if (ldtm_format_amiga_volume(path, 0U, "LD_OFS") != 0 ||
        ldtm_validate_amiga_volume(path, 0U) != 0) {
        (void)fprintf(stderr, "2 GiB OFS qualification fixture failed\n");
        (void)unlink(path);
        return 1;
    }
    if (reset_two_gib_sparse_image(path) != 0 ||
        ldtm_format_amiga_volume(path, 1U, "LD_FFS") != 0 ||
        ldtm_validate_amiga_volume(path, 1U) != 0) {
        (void)fprintf(stderr, "2 GiB FFS qualification fixture failed\n");
        (void)unlink(path);
        return 1;
    }

    const LdtmFilesystemSpec *sfs = ldtm_find_spec("sfs");
    const LdtmFilesystemSpec *pfs3 = ldtm_find_spec("pfs3");
    if (sfs == NULL || pfs3 == NULL) {
        (void)unlink(path);
        return 1;
    }
    const LdtmFragmentProfile sfs_profile = ldtm_fragment_profile(sfs);
    const LdtmFragmentProfile pfs3_profile = ldtm_fragment_profile(pfs3);

    if (reset_two_gib_sparse_image(path) != 0 ||
        ldtm_format_amiga_volume(path, 1U, "LD_SFS") != 0 ||
        ldtm_populate_amiga_volume(path, 1U, &sfs_profile) != 0 ||
        ldtm_verify_amiga_payload(
            path, 1U, &sfs_profile, detail, sizeof(detail)) != 0) {
        (void)fprintf(stderr, "2 GiB SFS qualification fixture failed: %s\n",
                      detail[0] != '\0' ? detail : "unknown");
        (void)unlink(path);
        return 1;
    }
    if (reset_two_gib_sparse_image(path) != 0 ||
        ldtm_format_amiga_volume(path, 1U, "LD_PFS3") != 0 ||
        ldtm_populate_amiga_volume(path, 1U, &pfs3_profile) != 0 ||
        ldtm_verify_amiga_payload(
            path, 1U, &pfs3_profile, detail, sizeof(detail)) != 0) {
        (void)fprintf(stderr, "2 GiB PFS3 qualification fixture failed: %s\n",
                      detail[0] != '\0' ? detail : "unknown");
        (void)unlink(path);
        return 1;
    }
    if (reset_two_gib_sparse_image(path) != 0 ||
        ldtm_format_apfs_volume(path) != 0 ||
        ldtm_verify_apfs_payload(path, detail, sizeof(detail)) != 0) {
        (void)fprintf(stderr, "2 GiB APFS qualification fixture failed: %s\n",
                      detail[0] != '\0' ? detail : "unknown");
        (void)unlink(path);
        return 1;
    }
    return unlink(path) == 0 ? 0 : 1;
}

int main(void) {
    char script[8192];
    const LdtmFilesystemSpec *fat12 = ldtm_find_spec("fat12");
    const LdtmFilesystemSpec *fat16 = ldtm_find_spec("fat16");
    const LdtmFilesystemSpec *ofs = ldtm_find_spec("ofs");
    const LdtmFilesystemSpec *ffs = ldtm_find_spec("ffs");
    const LdtmFilesystemSpec *sfs = ldtm_find_spec("sfs");
    const LdtmFilesystemSpec *pfs3 = ldtm_find_spec("pfs3");
    const LdtmFilesystemSpec *ufs = ldtm_find_spec("ufs");
    const LdtmFilesystemSpec *zfs = ldtm_find_spec("zfs");
    const LdtmFilesystemSpec *apfs = ldtm_find_spec("apfs");
    LdtmFragmentProfile small;
    LdtmFragmentProfile normal;
    LdtmFragmentProfile sfs_profile;
    LdtmFragmentProfile pfs3_profile;
    size_t count = 0U;
    const char *cursor;

    CHECK(ldtm_spec_count() == 21U);
    CHECK(ldtm_allocated_capacity_bytes() == UINT64_C(41215) * LDTM_MIB);
    CHECK(ldtm_required_capacity_bytes() == (UINT64_C(41215) * LDTM_MIB) + LDTM_GIB);
    CHECK(fat12 != NULL && fat16 != NULL && ofs != NULL && ffs != NULL && sfs != NULL && pfs3 != NULL);
    CHECK(ufs != NULL && zfs != NULL && apfs != NULL);

    small = ldtm_fragment_profile(fat12);
    normal = ldtm_fragment_profile(fat16);
    sfs_profile = ldtm_fragment_profile(sfs);
    pfs3_profile = ldtm_fragment_profile(pfs3);
    CHECK(ldtm_target_payload_bytes(fat12) == UINT64_C(4) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(fat16) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(ofs) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(ffs) == UINT64_C(200) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(sfs) == UINT64_C(25) * LDTM_MIB);
    CHECK(ldtm_target_payload_bytes(pfs3) == UINT64_C(25) * LDTM_MIB);
    CHECK(small.directory_initial == 128U && small.directory_second == 128U);
    CHECK(normal.chunks == 100U && normal.directory_initial == 4096U && normal.directory_second == 4096U);
    CHECK(sfs_profile.files == 1U && sfs_profile.chunks == 100U && sfs_profile.chunk_kib == 256U);
    CHECK(sfs_profile.directory_initial == 0U && sfs_profile.directory_second == 0U);
    CHECK(pfs3_profile.files == 1U && pfs3_profile.chunks == 100U &&
          pfs3_profile.chunk_kib == 256U);
    CHECK(pfs3_profile.directory_initial == 0U && pfs3_profile.directory_second == 0U);

    CHECK(ofs->creator == LDTM_CREATOR_AFFS);
    CHECK(ffs->creator == LDTM_CREATOR_AFFS);
    CHECK(sfs->creator == LDTM_CREATOR_AFFS);
    CHECK(strcmp(ldtm_creator_program(ofs), "/usr/lib/linux-defragger/test-media-mkfs-ofs") == 0);
    CHECK(strcmp(ldtm_creator_program(ffs), "/usr/lib/linux-defragger/test-media-mkfs-ffs") == 0);
    CHECK(ldtm_creator_program(sfs) == NULL);
    CHECK(strcmp(ldtm_creator_display_name(fat12), "mkfs.fat") == 0);
    CHECK(strcmp(ldtm_creator_display_name(ofs), "Built-in native") == 0);
    CHECK(strcmp(ldtm_creator_display_name(ffs), "Built-in native") == 0);
    CHECK(strcmp(ldtm_creator_display_name(sfs), "Built-in native") == 0);
    CHECK(strcmp(ldtm_creator_display_name(pfs3), "Built-in native") == 0);
    CHECK(strcmp(ldtm_creator_display_name(apfs), "Built-in native") == 0);
    CHECK(strcmp(ldtm_creator_display_name(NULL), "Unavailable") == 0);
    CHECK(strstr(sfs->note, "SFS0") != NULL && strstr(sfs->note, "100 extents") != NULL);
    CHECK(pfs3->creator == LDTM_CREATOR_PFS3 && strstr(pfs3->note, "PFS3") != NULL);
    CHECK(strcmp(ldtm_creator_program(ufs), "makefs") == 0);
    CHECK(ufs->package_hint != NULL && strcmp(ufs->package_hint, "makefs") == 0);
    CHECK(strstr(ufs->note, "UFS2") != NULL &&
          strstr(ufs->note, "exact allocation/fragmentation") != NULL);
    CHECK(zfs->package_hint != NULL && strcmp(zfs->package_hint, "zfsutils-linux") == 0);
    CHECK(strstr(zfs->note, "ZFS v28") != NULL &&
          strstr(zfs->note, "native exact analyser") != NULL);
    CHECK(fat12->size_mib == 255U);
    CHECK(fat16->size_mib == 2048U);
    CHECK(ofs->size_mib == 2048U && ffs->size_mib == 2048U);
    CHECK(sfs->size_mib == 2048U && pfs3->size_mib == 2048U);
    CHECK(zfs->size_mib == 2048U && apfs->size_mib == 2048U);
    CHECK(apfs->creator == LDTM_CREATOR_APFS && ldtm_creator_program(apfs) == NULL);
    CHECK(ldtm_spec_creator_available(apfs, script, sizeof(script)) == 1);
    CHECK(strstr(script, "Built-in raw C creator") != NULL);
    CHECK(ldtm_is_reserved_partition_label("LD_SFS") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_PFS3") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_APFS") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_OFS") == 0);
    CHECK(ldtm_is_reserved_partition_label("LD_HFSPLUS") == 0);
    CHECK(test_amiga_formatters_and_payload() == 0);
    CHECK(test_two_gib_first_party_media_geometry() == 0);
    CHECK(test_sfs_formatter_and_payload() == 0);
    CHECK(test_pfs3_formatter_and_payload() == 0);
    CHECK(test_apfs_formatter_and_payload() == 0);

    CHECK(ldtm_transport_is_field_media(0, "mmc") == 1);
    CHECK(ldtm_transport_is_field_media(0, "usb") == 1);
    CHECK(ldtm_transport_is_field_media(0, "sd") == 1);
    CHECK(ldtm_transport_is_field_media(1, "unknown") == 1);
    CHECK(ldtm_transport_is_field_media(0, "nvme") == 0);
    CHECK(ldtm_transport_is_field_media(0, "sata") == 0);
    CHECK(ldtm_transport_is_field_media(0, "sas") == 0);
    CHECK(ldtm_transport_is_field_media(0, "virtio") == 0);
    CHECK(ldtm_transport_is_field_media(0, "") == 0);
    CHECK(ldtm_transport_is_field_media(0, NULL) == 0);

    CHECK(ldtm_build_sfdisk_script(script, sizeof(script)) == 0);
    CHECK(strstr(script, "label: gpt") != NULL);
    CHECK(strstr(script, "size=255MiB, type=linux, name=\"LD_FAT12\"") != NULL);
    CHECK(strstr(script, "size=2048MiB, type=linux, name=\"LD_SFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_FAT12\"") != NULL);
    CHECK(strstr(script, "name=\"LD_OFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_FFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_PFS3\"") != NULL);
    CHECK(strstr(script, "name=\"LD_UFS\"") != NULL);
    CHECK(strstr(script, "name=\"LD_ZFS\"") != NULL);
    CHECK(strstr(script, "type=swap, name=\"LD_SWAP\"") != NULL);
    cursor = script;
    while ((cursor = strstr(cursor, "name=\"LD_")) != NULL) { ++count; cursor += 6; }
    CHECK(count == 21U);

    puts("test-media core tests passed");
    return 0;
}
