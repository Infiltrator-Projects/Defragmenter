// SPDX-License-Identifier: GPL-3.0-or-later
#include "minix_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE_BYTES 65536U
#define SUPER_OFFSET 1024U
#define LARGE_BLOCK_SIZE 4096U
#define LARGE_ZONE_COUNT 262144U
#define LARGE_FIRST_DATA_ZONE 12U
#define LARGE_IMAGE_BYTES ((uint64_t)LARGE_BLOCK_SIZE * LARGE_ZONE_COUNT)

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                          __LINE__, #expr);                                   \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

static void put16(uint8_t *p, uint16_t value, int little)
{
    if (little != 0) {
        p[0] = (uint8_t)(value & 0xffU);
        p[1] = (uint8_t)(value >> 8);
    } else {
        p[0] = (uint8_t)(value >> 8);
        p[1] = (uint8_t)(value & 0xffU);
    }
}

static void put32(uint8_t *p, uint32_t value, int little)
{
    if (little != 0) {
        p[0] = (uint8_t)(value & 0xffU);
        p[1] = (uint8_t)((value >> 8) & 0xffU);
        p[2] = (uint8_t)((value >> 16) & 0xffU);
        p[3] = (uint8_t)(value >> 24);
    } else {
        p[0] = (uint8_t)(value >> 24);
        p[1] = (uint8_t)((value >> 16) & 0xffU);
        p[2] = (uint8_t)((value >> 8) & 0xffU);
        p[3] = (uint8_t)(value & 0xffU);
    }
}

static int write_image(const uint8_t *image, char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-minix-XXXXXX");
    const int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    const ssize_t count = write(fd, image, IMAGE_BYTES);
    const int close_result = close(fd);
    if (count != (ssize_t)IMAGE_BYTES || close_result != 0) {
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static void make_legacy(uint8_t image[IMAGE_BYTES], uint16_t magic,
                        unsigned int version, int little)
{
    memset(image, 0, IMAGE_BYTES);
    uint8_t *sb = image + SUPER_OFFSET;
    put16(sb + 0U, 128U, little);
    put16(sb + 2U, 4096U, little);
    put16(sb + 4U, 1U, little);
    put16(sb + 6U, 1U, little);
    put16(sb + 8U, 10U, little);
    put16(sb + 10U, 0U, little);
    put32(sb + 12U, 0x00100000U, little);
    put16(sb + 16U, magic, little);
    if (version == 2U)
        put32(sb + 20U, 5000U, little);
}

static void make_v3(uint8_t image[IMAGE_BYTES], int little)
{
    memset(image, 0, IMAGE_BYTES);
    uint8_t *sb = image + SUPER_OFFSET;
    put32(sb + 0U, 300U, little);
    put16(sb + 6U, 1U, little);
    put16(sb + 8U, 2U, little);
    put16(sb + 10U, 20U, little);
    put16(sb + 12U, 1U, little);
    put32(sb + 16U, 0x01000000U, little);
    put32(sb + 20U, 12000U, little);
    put16(sb + 24U, 0x4d5aU, little);
    put16(sb + 28U, 4096U, little);
}

static int expect_summary(const uint8_t image[IMAGE_BYTES],
                          const char *variant, const char *order,
                          unsigned int version, uint32_t block_size,
                          uint32_t zones)
{
    char path[64];
    if (write_image(image, path) != 0)
        return 1;

    MinixSummary summary;
    char error[160];
    const int result = minix_read_summary(path, &summary, error, sizeof(error));
    (void)unlink(path);
    if (result != 0) {
        (void)fprintf(stderr, "parse failed: %s\n", error);
        return 1;
    }
    if (strcmp(minix_variant_name(&summary), variant) != 0 ||
        strcmp(minix_byte_order_name(&summary), order) != 0 ||
        summary.version != version || summary.block_size != block_size ||
        summary.zone_count != zones || summary.inode_count == 0U ||
        summary.imap_blocks == 0U || summary.zmap_blocks == 0U ||
        summary.first_data_zone == 0U || summary.zone_size < block_size) {
        (void)fprintf(stderr, "unexpected Minix summary for %s/%s\n", variant, order);
        return 1;
    }
    return 0;
}

static void set_little_bit(uint8_t *map, unsigned int bit)
{
    map[bit >> 3U] |= (uint8_t)(1U << (bit & 7U));
}

static void make_exact_v3(uint8_t image[IMAGE_BYTES])
{
    memset(image, 0, IMAGE_BYTES);
    uint8_t *sb = image + SUPER_OFFSET;
    put32(sb + 0U, 16U, 1);
    put16(sb + 6U, 1U, 1);
    put16(sb + 8U, 1U, 1);
    put16(sb + 10U, 8U, 1);
    put16(sb + 12U, 0U, 1);
    put32(sb + 16U, 0x01000000U, 1);
    put32(sb + 20U, 64U, 1);
    put16(sb + 24U, 0x4d5aU, 1);
    put16(sb + 28U, 1024U, 1);

    uint8_t *imap = image + 2048U;
    uint8_t *zmap = image + 3072U;
    set_little_bit(imap, 0U);
    set_little_bit(imap, 1U);
    set_little_bit(imap, 2U);
    set_little_bit(imap, 3U);
    set_little_bit(zmap, 0U);

    const unsigned int zones[] = {8U, 10U, 11U, 13U, 14U};
    for (size_t index = 0U; index < sizeof(zones) / sizeof(zones[0]); ++index)
        set_little_bit(zmap, zones[index] - 8U + 1U);

    uint8_t *inodes = image + 4096U;
    put16(inodes + 0U, 0040755U, 1);
    put16(inodes + 2U, 1U, 1);
    put32(inodes + 8U, 2048U, 1);
    put32(inodes + 24U, 8U, 1);
    put32(inodes + 28U, 10U, 1);

    inodes += 64U;
    put16(inodes + 0U, 0100644U, 1);
    put16(inodes + 2U, 1U, 1);
    put32(inodes + 8U, 2048U, 1);
    put32(inodes + 24U, 11U, 1);
    put32(inodes + 28U, 13U, 1);

    inodes += 64U;
    put16(inodes + 0U, 0100644U, 1);
    put16(inodes + 2U, 1U, 1);
    put32(inodes + 8U, 1024U, 1);
    put32(inodes + 24U, 14U, 1);
}

static void test_exact_analysis(void)
{
    uint8_t image[IMAGE_BYTES];
    make_exact_v3(image);

    char path[64];
    CHECK(write_image(image, path) == 0);
    MinixAnalysis analysis;
    MinixMapCell cells[8];
    char error[160];
    CHECK(minix_analyse(path, &analysis, cells, 8U, error, sizeof(error)) == 0);
    CHECK(analysis.summary.version == 3U);
    CHECK(analysis.free_zones == 51U);
    CHECK(analysis.used_zones == 13U);
    CHECK(analysis.regular_files == 2U);
    CHECK(analysis.directories == 1U);
    CHECK(analysis.fragmented_files == 1U);
    CHECK(analysis.fragmented_directories == 1U);

    uint64_t free_zones = 0U;
    uint64_t used_zones = 0U;
    uint64_t fragmented_zones = 0U;
    uint64_t directory_zones = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        free_zones += cells[index].free_count;
        used_zones += cells[index].used_count;
        fragmented_zones += cells[index].fragmented_count;
        directory_zones += cells[index].directory_count;
    }
    CHECK(free_zones == 51U);
    CHECK(used_zones == 13U);
    CHECK(fragmented_zones == 4U);
    CHECK(directory_zones == 2U);

    image[3072U] &= (uint8_t)~(1U << 4U); /* zone 11 becomes unallocated */
    CHECK(unlink(path) == 0);
    CHECK(write_image(image, path) == 0);
    CHECK(minix_analyse(path, &analysis, NULL, 0U, error, sizeof(error)) != 0);
    CHECK(strstr(error, "unallocated") != NULL);
    CHECK(unlink(path) == 0);
}

static void test_full_size_map_analysis(void)
{
    char path[64];
    (void)snprintf(path, sizeof(path), "/tmp/linux-defragger-minix-large-XXXXXX");
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)LARGE_IMAGE_BYTES) == 0);

    uint8_t superblock[64] = {0};
    put32(superblock + 0U, 16U, 1);
    put16(superblock + 6U, 1U, 1);
    put16(superblock + 8U, 8U, 1);
    put16(superblock + 10U, LARGE_FIRST_DATA_ZONE, 1);
    put16(superblock + 12U, 0U, 1);
    put32(superblock + 16U, 0x01000000U, 1);
    put32(superblock + 20U, LARGE_ZONE_COUNT, 1);
    put16(superblock + 24U, 0x4d5aU, 1);
    put16(superblock + 28U, LARGE_BLOCK_SIZE, 1);
    CHECK(pwrite(fd, superblock, sizeof(superblock), SUPER_OFFSET) ==
          (ssize_t)sizeof(superblock));

    uint8_t imap[LARGE_BLOCK_SIZE] = {0};
    set_little_bit(imap, 0U);
    CHECK(pwrite(fd, imap, sizeof(imap), 2U * LARGE_BLOCK_SIZE) ==
          (ssize_t)sizeof(imap));

    const size_t zmap_bytes = 8U * LARGE_BLOCK_SIZE;
    uint8_t *zmap = calloc(1U, zmap_bytes);
    CHECK(zmap != NULL);
    set_little_bit(zmap, 0U);
    CHECK(pwrite(fd, zmap, zmap_bytes, 3U * LARGE_BLOCK_SIZE) ==
          (ssize_t)zmap_bytes);
    free(zmap);
    CHECK(close(fd) == 0);

    MinixMapCell *cells = calloc(LARGE_ZONE_COUNT, sizeof(*cells));
    CHECK(cells != NULL);
    MinixAnalysis analysis;
    char error[160];
    CHECK(minix_analyse(path, &analysis, cells, LARGE_ZONE_COUNT,
                        error, sizeof(error)) == 0);
    CHECK(analysis.total_units == LARGE_ZONE_COUNT);
    CHECK(analysis.used_zones == LARGE_FIRST_DATA_ZONE);
    CHECK(analysis.free_zones ==
          LARGE_ZONE_COUNT - LARGE_FIRST_DATA_ZONE);
    CHECK(cells[0].used_count == 1U);
    CHECK(cells[LARGE_FIRST_DATA_ZONE - 1U].used_count == 1U);
    CHECK(cells[LARGE_FIRST_DATA_ZONE].free_count == 1U);
    CHECK(cells[LARGE_ZONE_COUNT - 1U].free_count == 1U);

    free(cells);
    CHECK(unlink(path) == 0);
}



static int write_fixture_path(const char *path)
{
    uint8_t image[IMAGE_BYTES];
    make_exact_v3(image);
    const unsigned int zones[] = {8U, 10U, 11U, 13U, 14U};
    for (size_t index = 0U; index < sizeof(zones) / sizeof(zones[0]); ++index)
        memset(image + (size_t)zones[index] * 1024U,
               (int)(0x30U + zones[index]), 1024U);

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    const ssize_t count = write(fd, image, sizeof(image));
    const int close_result = close(fd);
    return count == (ssize_t)sizeof(image) && close_result == 0 ? 0 : -1;
}

static void make_stage_path(char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-minix-stage-XXXXXX");
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
}

static void test_native_relayout(void)
{
    uint8_t image[IMAGE_BYTES];
    make_exact_v3(image);
    const unsigned int zones[] = {8U, 10U, 11U, 13U, 14U};
    for (size_t index = 0U; index < sizeof(zones) / sizeof(zones[0]); ++index)
        memset(image + (size_t)zones[index] * 1024U,
               (int)(0x30U + zones[index]), 1024U);

    char source[64];
    CHECK(write_image(image, source) == 0);
    char error[256];
    uint64_t committed = 0U;

    char packed[64];
    make_stage_path(packed);
    CHECK(minix_build_stage(source, packed, false, 10U, &committed,
                            error, sizeof(error)) == 0);
    CHECK(committed == IMAGE_BYTES);
    CHECK(minix_verify_layout(packed, false, 10U,
                              error, sizeof(error)) == 0);
    MinixAnalysis analysis;
    CHECK(minix_analyse(packed, &analysis, NULL, 0U,
                        error, sizeof(error)) == 0);
    CHECK(analysis.fragmented_files == 0U);
    CHECK(analysis.fragmented_directories == 0U);
    CHECK(unlink(packed) == 0);

    char growth[64];
    make_stage_path(growth);
    committed = 0U;
    CHECK(minix_build_stage(source, growth, true, 10U, &committed,
                            error, sizeof(error)) == 0);
    CHECK(committed == IMAGE_BYTES);
    CHECK(minix_verify_layout(growth, true, 10U,
                              error, sizeof(error)) == 0);
    CHECK(minix_verify_layout(growth, false, 10U,
                              error, sizeof(error)) != 0);
    CHECK(unlink(growth) == 0);
    CHECK(unlink(source) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--write-fixture") == 0 &&
        strcmp(argv[3], "fragmented") == 0)
        return write_fixture_path(argv[2]) == 0 ? 0 : 1;
    if (argc != 1) {
        (void)fprintf(stderr,
                      "Usage: %s [--write-fixture PATH fragmented]\n",
                      argv[0]);
        return 2;
    }

    uint8_t image[IMAGE_BYTES];

    make_legacy(image, 0x137fU, 1U, 1);
    if (expect_summary(image, "v1", "little", 1U, 1024U, 4096U) != 0)
        return 1;

    make_legacy(image, 0x138fU, 1U, 0);
    if (expect_summary(image, "v1-30char", "big", 1U, 1024U, 4096U) != 0)
        return 1;

    make_legacy(image, 0x2468U, 2U, 1);
    if (expect_summary(image, "v2", "little", 2U, 1024U, 5000U) != 0)
        return 1;

    make_legacy(image, 0x2478U, 2U, 0);
    if (expect_summary(image, "v2-30char", "big", 2U, 1024U, 5000U) != 0)
        return 1;

    make_v3(image, 1);
    if (expect_summary(image, "v3", "little", 3U, 4096U, 12000U) != 0)
        return 1;

    make_v3(image, 0);
    if (expect_summary(image, "v3", "big", 3U, 4096U, 12000U) != 0)
        return 1;

    memset(image, 0, IMAGE_BYTES);
    char path[64];
    if (write_image(image, path) != 0)
        return 1;
    MinixSummary summary;
    if (minix_read_summary(path, &summary, NULL, 0U) == 0) {
        (void)unlink(path);
        (void)fprintf(stderr, "invalid Minix magic was accepted\n");
        return 1;
    }
    (void)unlink(path);

    make_v3(image, 1);
    put32(image + SUPER_OFFSET + 20U, 10U, 1);
    if (write_image(image, path) != 0)
        return 1;
    if (minix_read_summary(path, &summary, NULL, 0U) == 0) {
        (void)unlink(path);
        (void)fprintf(stderr, "invalid Minix geometry was accepted\n");
        return 1;
    }
    (void)unlink(path);

    test_exact_analysis();
    test_native_relayout();
    test_full_size_map_analysis();
    (void)puts("Minix summary, exact analysis and native relayout tests passed");
    return 0;
}
