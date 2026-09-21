// SPDX-License-Identifier: GPL-3.0-or-later
#include "ufs_native.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE_BYTES 300000
#define UFS_DISK_STRUCT_BYTES 1376U
#define UFS_DISK_MAGIC_OFFSET 1372U

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                          __LINE__, #expr);                                   \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

static void write_all(int fd, const void *buffer, size_t length, off_t offset)
{
    const uint8_t *bytes = buffer;
    size_t done = 0U;
    while (done < length) {
        const ssize_t count = pwrite(fd, bytes + done, length - done,
                                     offset + (off_t)done);
        CHECK(count > 0);
        done += (size_t)count;
    }
}

static void clear_image(int fd)
{
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, IMAGE_BYTES) == 0);
}

static void put_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *data, uint64_t value)
{
    for (unsigned int index = 0U; index < 8U; ++index)
        data[index] = (uint8_t)(value >> (index * 8U));
}

static void check_variant(int fd, const char *path, const uint8_t magic[4],
                          uint64_t candidate,
                          LdUfsVariant expected, const char *name,
                          const char *byte_order, unsigned int version)
{
    clear_image(fd);
    const uint64_t position = UFS_DISK_MAGIC_OFFSET;
    write_all(fd, magic, 4U, (off_t)(candidate + position));

    LdUfsSummary summary;
    char error[256];
    CHECK(ufs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.variant == expected);
    CHECK(summary.superblock_offset == candidate);
    CHECK(summary.magic_offset == candidate + position);
    CHECK(strcmp(ufs_variant_name(&summary), name) == 0);
    CHECK(strcmp(ufs_byte_order_name(&summary), byte_order) == 0);
    CHECK(ufs_version(&summary) == version);
    CHECK(!summary.allocation_totals_known);
    CHECK(!summary.cylinder_geometry_known);
    CHECK(ufs_probe(path));
}

static void test_ufs2_recorded_allocation(int fd, const char *path)
{
    clear_image(fd);
    uint8_t superblock[UFS_DISK_STRUCT_BYTES];
    memset(superblock, 0, sizeof(superblock));

    put_le32(superblock + 48U, 8192U);
    put_le32(superblock + 52U, 1024U);
    put_le32(superblock + 56U, 8U);
    put_le32(superblock + 116U, 1024U);
    put_le32(superblock + 120U, 32U);
    put_le64(superblock + 1016U, 100U);
    put_le64(superblock + 1032U, 3U);
    put_le64(superblock + 1080U, 1000U);
    put_le64(superblock + 1088U, 900U);
    static const uint8_t ufs2_le[4] = {0x19U, 0x01U, 0x54U, 0x19U};
    memcpy(superblock + UFS_DISK_MAGIC_OFFSET, ufs2_le, sizeof(ufs2_le));
    write_all(fd, superblock, sizeof(superblock), 65536);

    LdUfsSummary summary;
    char error[256];
    CHECK(ufs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.variant == LD_UFS_VARIANT_UFS2_LE);
    CHECK(summary.allocation_totals_known);
    CHECK(!summary.cylinder_geometry_known);
    CHECK(summary.block_size == 8192U);
    CHECK(summary.fragment_size == 1024U);
    CHECK(summary.fragments_per_block == 8U);
    CHECK(summary.filesystem_fragments == 1000U);
    CHECK(summary.data_fragments == 900U);
    CHECK(summary.free_blocks == 100U);
    CHECK(summary.free_fragments == 3U);
    CHECK(ufs_recorded_data_bytes(&summary) == UINT64_C(921600));
    CHECK(ufs_recorded_free_bytes(&summary) == UINT64_C(822272));
    CHECK(ufs_recorded_used_bytes(&summary) == UINT64_C(99328));
}

static void set_free(uint8_t *map, uint32_t bit)
{
    map[bit >> 3U] |= (uint8_t)(1U << (bit & 7U));
}

static void write_exact_superblock(int fd)
{
    uint8_t superblock[UFS_DISK_STRUCT_BYTES];
    memset(superblock, 0, sizeof(superblock));
    put_le32(superblock + 8U, 64U);
    put_le32(superblock + 12U, 72U);
    put_le32(superblock + 16U, 80U);
    put_le32(superblock + 20U, 96U);
    put_le32(superblock + 44U, 2U);
    put_le32(superblock + 48U, 8192U);
    put_le32(superblock + 52U, 1024U);
    put_le32(superblock + 56U, 8U);
    put_le32(superblock + 116U, 1024U);
    put_le32(superblock + 120U, 32U);
    put_le32(superblock + 160U, 1024U);
    put_le32(superblock + 184U, 64U);
    put_le32(superblock + 188U, 128U);
    put_le64(superblock + 1016U, 5U);
    put_le64(superblock + 1032U, 0U);
    put_le64(superblock + 1080U, 240U);
    put_le64(superblock + 1088U, 180U);
    put_le32(superblock + UFS_DISK_MAGIC_OFFSET, 0x19540119U);
    write_all(fd, superblock, sizeof(superblock), 65536);
}

static void write_cylinder_group(int fd, uint32_t group, uint32_t fragments,
                                 uint32_t *free_remaining)
{
    uint8_t cg[1024];
    memset(cg, 0, sizeof(cg));
    put_le32(cg + 4U, 0x00090255U);
    put_le32(cg + 12U, group);
    put_le32(cg + 20U, fragments);
    put_le32(cg + 92U, 168U);
    put_le32(cg + 96U, 176U);
    for (uint32_t local = 80U; local < fragments && *free_remaining != 0U;
         ++local) {
        set_free(cg + 176U, local);
        (*free_remaining)--;
    }
    const uint64_t group_base = (uint64_t)group * 128U;
    const uint64_t offset = (group_base + 72U) * 1024U;
    write_all(fd, cg, sizeof(cg), (off_t)offset);
}

static void test_ufs2_exact_allocation(int fd, const char *path)
{
    clear_image(fd);
    write_exact_superblock(fd);
    uint32_t free_remaining = 40U;
    write_cylinder_group(fd, 0U, 128U, &free_remaining);
    write_cylinder_group(fd, 1U, 112U, &free_remaining);
    CHECK(free_remaining == 0U);

    LdUfsAnalysis analysis;
    LdUfsMapCell cells[8];
    char error[256];
    CHECK(ufs_analyse_allocation(path, &analysis, cells, 8U,
                                 error, sizeof(error)) == 0);
    CHECK(analysis.summary.cylinder_geometry_known);
    CHECK(analysis.summary.cylinder_groups == 2U);
    CHECK(analysis.summary.fragments_per_group == 128U);
    CHECK(analysis.free_fragments_exact == 40U);
    CHECK(analysis.used_fragments_exact == 200U);

    uint64_t free_total = 0U;
    uint64_t used_total = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        free_total += cells[index].free_count;
        used_total += cells[index].used_count;
    }
    CHECK(free_total == 40U);
    CHECK(used_total == 200U);

    uint8_t byte = 0U;
    CHECK(pread(fd, &byte, 1U, (off_t)(72U * 1024U + 176U + 10U)) == 1);
    byte ^= 0x01U;
    write_all(fd, &byte, 1U, (off_t)(72U * 1024U + 176U + 10U));
    CHECK(ufs_analyse_allocation(path, &analysis, NULL, 0U,
                                 error, sizeof(error)) != 0);
    CHECK(strstr(error, "disagrees") != NULL);
}

int main(void)
{
    char path[] = "/tmp/linux-defragger-ufs-test-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);

    static const uint8_t ufs1_le[4] = {0x54U, 0x19U, 0x01U, 0x00U};
    static const uint8_t ufs1_be[4] = {0x00U, 0x01U, 0x19U, 0x54U};
    static const uint8_t ufs2_le[4] = {0x19U, 0x01U, 0x54U, 0x19U};
    static const uint8_t ufs2_be[4] = {0x19U, 0x54U, 0x01U, 0x19U};

    check_variant(fd, path, ufs1_le, 8192U, LD_UFS_VARIANT_UFS1_LE,
                  "ufs1-le", "little", 1U);
    check_variant(fd, path, ufs1_be, 65536U, LD_UFS_VARIANT_UFS1_BE,
                  "ufs1-be", "big", 1U);
    check_variant(fd, path, ufs2_le, 262144U, LD_UFS_VARIANT_UFS2_LE,
                  "ufs2-le", "little", 2U);
    check_variant(fd, path, ufs2_be, 8192U, LD_UFS_VARIANT_UFS2_BE,
                  "ufs2-be", "big", 2U);

    test_ufs2_recorded_allocation(fd, path);
    test_ufs2_exact_allocation(fd, path);

    clear_image(fd);
    static const uint8_t junk[4] = {0xdeU, 0xadU, 0xbeU, 0xefU};
    write_all(fd, junk, sizeof(junk), 8192 + 100);
    LdUfsSummary summary;
    char error[256];
    CHECK(ufs_read_summary(path, &summary, error, sizeof(error)) != 0);
    CHECK(!ufs_probe(path));
    CHECK(strstr(error, "not a recognised UFS volume") != NULL);

    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
    (void)puts("UFS native summary and exact UFS2 allocation tests passed");
    return 0;
}
