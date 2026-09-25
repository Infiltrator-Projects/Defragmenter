// SPDX-License-Identifier: GPL-3.0-or-later
#include "pfs3_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_SECTOR 512U
#define TEST_SECTORS 256U
#define TEST_BYTES (TEST_SECTOR * TEST_SECTORS)
#define ROOT_SECTOR 2U
#define LAST_RESERVED 65U
#define FIRST_DATA 66U
#define EXTENSION_SECTOR 4U
#define BITMAP_INDEX_SECTOR 6U
#define BITMAP_SECTOR 8U
#define ANODE_INDEX_SECTOR 10U
#define ANODE_BLOCK_SECTOR 12U
#define ROOT_DIR_SECTOR 14U

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8U);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24U);
    p[1] = (uint8_t)(v >> 16U);
    p[2] = (uint8_t)(v >> 8U);
    p[3] = (uint8_t)v;
}

static void bitmap_bit(uint8_t *base, uint32_t bit, int is_free)
{
    const uint32_t word = bit / 32U;
    const uint32_t within = bit % 32U;
    uint8_t *p = base + word * 4U;
    uint32_t value =
        ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) |
        ((uint32_t)p[2] << 8U) | p[3];
    const uint32_t mask = UINT32_C(0x80000000) >> within;
    if (is_free != 0)
        value |= mask;
    else
        value &= ~mask;
    put32(p, value);
}

static void make_root(uint8_t *image)
{
    uint8_t *root = image + ROOT_SECTOR * TEST_SECTOR;
    memset(root, 0, TEST_SECTOR);
    put32(root, UINT32_C(0x50465301));
    put32(root + 4U, UINT32_C(0x677));
    put32(root + 8U, UINT32_C(0x01020304));
    root[20U] = 7U;
    memcpy(root + 21U, "PFS3-TM", 7U);
    put32(root + 52U, LAST_RESERVED);
    put32(root + 56U, ROOT_SECTOR);
    put32(root + 60U, 25U);
    put16(root + 64U, 1024U);
    put16(root + 66U, 2U);
    put32(root + 68U, 187U);
    put32(root + 72U, 0U);
    put32(root + 76U, 0U);
    put32(root + 84U, TEST_SECTORS);
    put32(root + 88U, EXTENSION_SECTOR);
    put32(root + 96U, BITMAP_INDEX_SECTOR);
    put32(root + 116U, ANODE_INDEX_SECTOR);
}

static void make_reserved_bitmap(uint8_t *image)
{
    uint8_t *block = image + (ROOT_SECTOR + 1U) * TEST_SECTOR;
    memset(block, 0, TEST_SECTOR);
    put16(block, UINT16_C(0x424d));
    put32(block + 8U, 0U);
    for (uint32_t slot = 0U; slot < 32U; ++slot)
        bitmap_bit(block + 12U, slot, 1);
    for (uint32_t slot = 0U; slot <= 6U; ++slot)
        bitmap_bit(block + 12U, slot, 0);
}

static void make_extension(uint8_t *image)
{
    uint8_t *block = image + EXTENSION_SECTOR * TEST_SECTOR;
    memset(block, 0, 1024U);
    put16(block, UINT16_C(0x4558));
    put32(block + 4U, UINT32_C(0x01020304));
    put32(block + 28U, 0U);
}

static void make_bitmap_index(uint8_t *image)
{
    uint8_t *block = image + BITMAP_INDEX_SECTOR * TEST_SECTOR;
    memset(block, 0, 1024U);
    put16(block, UINT16_C(0x4d49));
    put32(block + 4U, UINT32_C(0x01020304));
    put32(block + 8U, 0U);
    put32(block + 12U, BITMAP_SECTOR);
}

static void make_normal_bitmap(uint8_t *image, int fragmented)
{
    uint8_t *block = image + BITMAP_SECTOR * TEST_SECTOR;
    memset(block, 0, 1024U);
    put16(block, UINT16_C(0x424d));
    put32(block + 4U, UINT32_C(0x01020304));
    put32(block + 8U, 0U);
    for (uint32_t sector = FIRST_DATA; sector < TEST_SECTORS; ++sector)
        bitmap_bit(block + 12U, sector - FIRST_DATA, 1);
    if (fragmented != 0) {
        bitmap_bit(block + 12U, 100U - FIRST_DATA, 0);
        bitmap_bit(block + 12U, 110U - FIRST_DATA, 0);
        bitmap_bit(block + 12U, 111U - FIRST_DATA, 0);
    } else {
        bitmap_bit(block + 12U, 66U - FIRST_DATA, 0);
        bitmap_bit(block + 12U, 67U - FIRST_DATA, 0);
        bitmap_bit(block + 12U, 68U - FIRST_DATA, 0);
    }
}

static void make_anode_index(uint8_t *image)
{
    uint8_t *block = image + ANODE_INDEX_SECTOR * TEST_SECTOR;
    memset(block, 0, 1024U);
    put16(block, UINT16_C(0x4942));
    put32(block + 4U, UINT32_C(0x01020304));
    put32(block + 8U, 0U);
    put32(block + 12U, ANODE_BLOCK_SECTOR);
}

static void put_anode(uint8_t *block, uint32_t number,
                      uint32_t clusters, uint32_t sector, uint32_t next)
{
    uint8_t *record = block + 16U + (size_t)number * 12U;
    put32(record, clusters);
    put32(record + 4U, sector);
    put32(record + 8U, next);
}

static void make_anodes(uint8_t *image, int fragmented)
{
    uint8_t *block = image + ANODE_BLOCK_SECTOR * TEST_SECTOR;
    memset(block, 0, 1024U);
    put16(block, UINT16_C(0x4142));
    put32(block + 4U, UINT32_C(0x01020304));
    put32(block + 8U, 0U);
    put_anode(block, 5U, 1U, ROOT_DIR_SECTOR, 0U);
    if (fragmented != 0) {
        put_anode(block, 6U, 1U, 100U, 7U);
        put_anode(block, 7U, 2U, 110U, 0U);
    } else {
        put_anode(block, 6U, 3U, 66U, 0U);
    }
}

static void make_root_directory(uint8_t *image)
{
    uint8_t *block = image + ROOT_DIR_SECTOR * TEST_SECTOR;
    memset(block, 0, 1024U);
    put16(block, UINT16_C(0x4442));
    put32(block + 4U, UINT32_C(0x01020304));
    put32(block + 12U, 5U);
    put32(block + 16U, 0U);
    uint8_t *entry = block + 20U;
    entry[0] = 26U;
    entry[1] = UINT8_C(0xfd);
    put32(entry + 2U, 6U);
    put32(entry + 6U, 3U * TEST_SECTOR);
    entry[17U] = 4U;
    memcpy(entry + 18U, "frag", 4U);
    entry[22U] = 0U;
    entry[24U] = 0U;
    entry[26U] = 0U;
}

static void fill_payload(uint8_t *sector, uint8_t value)
{
    for (size_t index = 0U; index < TEST_SECTOR; ++index)
        sector[index] = (uint8_t)(value + (uint8_t)index);
}

static void make_payload(uint8_t *image, int fragmented)
{
    uint8_t first[TEST_SECTOR];
    uint8_t second[TEST_SECTOR];
    uint8_t third[TEST_SECTOR];
    fill_payload(first, UINT8_C(0x11));
    fill_payload(second, UINT8_C(0x42));
    fill_payload(third, UINT8_C(0x93));
    const uint32_t p0 = fragmented != 0 ? 100U : 66U;
    const uint32_t p1 = fragmented != 0 ? 110U : 67U;
    const uint32_t p2 = fragmented != 0 ? 111U : 68U;
    memcpy(image + (size_t)p0 * TEST_SECTOR, first, TEST_SECTOR);
    memcpy(image + (size_t)p1 * TEST_SECTOR, second, TEST_SECTOR);
    memcpy(image + (size_t)p2 * TEST_SECTOR, third, TEST_SECTOR);
}

static void make_image(uint8_t *image, int fragmented)
{
    memset(image, 0, TEST_BYTES);
    make_root(image);
    make_reserved_bitmap(image);
    make_extension(image);
    make_bitmap_index(image);
    make_normal_bitmap(image, fragmented);
    make_anode_index(image);
    make_anodes(image, fragmented);
    make_root_directory(image);
    make_payload(image, fragmented);
}

static int write_image_path(const char *path, const uint8_t *image)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    size_t done = 0U;
    while (done < TEST_BYTES) {
        const ssize_t written = write(fd, image + done, TEST_BYTES - done);
        if (written <= 0) {
            (void)close(fd);
            return -1;
        }
        done += (size_t)written;
    }
    const int result = fsync(fd) == 0 && close(fd) == 0 ? 0 : -1;
    return result;
}

static int save_image(const uint8_t *image, char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-pfs3-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    size_t done = 0U;
    while (done < TEST_BYTES) {
        const ssize_t written = write(fd, image + done, TEST_BYTES - done);
        if (written <= 0) {
            (void)close(fd);
            (void)unlink(path);
            return -1;
        }
        done += (size_t)written;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static int make_stage_path(char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-pfs3-stage-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    if (close(fd) != 0 || unlink(path) != 0)
        return -1;
    return 0;
}

static int verify_payload(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    uint8_t actual[TEST_SECTOR];
    uint8_t expected[TEST_SECTOR];
    const uint8_t seeds[3] = {UINT8_C(0x11), UINT8_C(0x42), UINT8_C(0x93)};
    int result = 0;
    for (uint32_t index = 0U; index < 3U; ++index) {
        fill_payload(expected, seeds[index]);
        if (pread(fd, actual, sizeof(actual),
                  (off_t)(FIRST_DATA + index) * TEST_SECTOR) !=
                (ssize_t)sizeof(actual) ||
            memcmp(actual, expected, sizeof(actual)) != 0) {
            result = -1;
            break;
        }
    }
    (void)close(fd);
    return result;
}

int main(int argc, char **argv)
{
    uint8_t *image = malloc(TEST_BYTES);
    if (image == NULL)
        return 2;

    if (argc == 4 && strcmp(argv[1], "--write-fixture") == 0) {
        const int fragmented = strcmp(argv[3], "fragmented") == 0 ? 1 :
                               strcmp(argv[3], "contiguous") == 0 ? 0 : -1;
        if (fragmented < 0) {
            free(image);
            return 2;
        }
        make_image(image, fragmented);
        const int result = write_image_path(argv[2], image);
        free(image);
        return result == 0 ? 0 : 1;
    }

    make_image(image, 1);
    char source[64];
    if (save_image(image, source) != 0) {
        free(image);
        return 3;
    }

    char error[256] = {0};
    Pfs3Analysis analysis;
    Pfs3MapCell cells[16];
    if (!pfs3_probe(source) ||
        pfs3_analyse(source, &analysis, cells, 16U, error, sizeof(error)) != 0 ||
        analysis.regular_files != 1U || analysis.data_blocks != 3U ||
        analysis.fragmented_files != 1U || analysis.growth_10_satisfied ||
        analysis.block_size != TEST_SECTOR ||
        analysis.total_blocks != TEST_SECTORS) {
        (void)fprintf(stderr, "valid PFS3 fixture rejected: %s\n", error);
        (void)unlink(source);
        free(image);
        return 4;
    }

    char stage[64];
    uint64_t commit_bytes = 0U;
    if (make_stage_path(stage) != 0 ||
        pfs3_build_stage(source, stage, false, 10U, false,
                         &commit_bytes, error, sizeof(error)) != 0 ||
        commit_bytes != TEST_BYTES ||
        pfs3_verify_layout(stage, false, 10U, error, sizeof(error)) != 0 ||
        verify_payload(stage) != 0) {
        (void)fprintf(stderr, "PFS3 Defrag stage failed: %s\n", error);
        (void)unlink(source);
        (void)unlink(stage);
        free(image);
        return 5;
    }
    Pfs3Analysis staged;
    if (pfs3_analyse(stage, &staged, NULL, 0U, error, sizeof(error)) != 0 ||
        staged.fragmented_files != 0U) {
        (void)fprintf(stderr, "PFS3 Defrag verification failed: %s\n", error);
        (void)unlink(source);
        (void)unlink(stage);
        free(image);
        return 6;
    }

    (void)unlink(stage);
    if (make_stage_path(stage) != 0 ||
        pfs3_build_stage(source, stage, true, 10U, false,
                         &commit_bytes, error, sizeof(error)) != 0 ||
        pfs3_verify_layout(stage, true, 10U, error, sizeof(error)) != 0 ||
        pfs3_analyse(stage, &staged, NULL, 0U, error, sizeof(error)) != 0 ||
        !staged.growth_10_satisfied || verify_payload(stage) != 0) {
        (void)fprintf(stderr, "PFS3 Growth Defrag stage failed: %s\n", error);
        (void)unlink(source);
        (void)unlink(stage);
        free(image);
        return 7;
    }
    (void)unlink(stage);

    make_image(image, 1);
    put32(image + ROOT_SECTOR * TEST_SECTOR + 4U,
          UINT32_C(0x677) | UINT32_C(0x80));
    char invalid[64];
    if (save_image(image, invalid) != 0) {
        (void)unlink(source);
        free(image);
        return 8;
    }
    memset(error, 0, sizeof(error));
    if (pfs3_analyse(invalid, &analysis, NULL, 0U,
                     error, sizeof(error)) == 0) {
        (void)fprintf(stderr, "unsupported PFS3 superindex media was accepted\n");
        (void)unlink(source);
        (void)unlink(invalid);
        free(image);
        return 9;
    }
    (void)unlink(invalid);

    make_image(image, 1);
    put16(image + EXTENSION_SECTOR * TEST_SECTOR + 48U, 32U);
    if (save_image(image, invalid) != 0) {
        (void)unlink(source);
        free(image);
        return 10;
    }
    memset(error, 0, sizeof(error));
    if (pfs3_analyse(invalid, &analysis, NULL, 0U,
                     error, sizeof(error)) == 0) {
        (void)fprintf(stderr,
                      "PFS3 extension with an invalid roving bit was accepted\n");
        (void)unlink(source);
        (void)unlink(invalid);
        free(image);
        return 11;
    }
    (void)unlink(invalid);

    make_image(image, 1);
    {
        uint8_t *const entry =
            image + ROOT_DIR_SECTOR * TEST_SECTOR + PFS_DIR_HEADER;
        entry[0] = 24U;
        entry[24U] = 0U;
    }
    if (save_image(image, invalid) != 0) {
        (void)unlink(source);
        free(image);
        return 12;
    }
    memset(error, 0, sizeof(error));
    if (pfs3_analyse(invalid, &analysis, NULL, 0U,
                     error, sizeof(error)) == 0) {
        (void)fprintf(stderr,
                      "PFS3 directory entry overlapping its extension tail was accepted\n");
        (void)unlink(source);
        (void)unlink(invalid);
        free(image);
        return 13;
    }

    (void)unlink(source);
    (void)unlink(invalid);
    free(image);
    return 0;
}
