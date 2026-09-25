// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfs_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCKS 128U
#define TEST_BYTES (TEST_BLOCK_SIZE * TEST_BLOCKS)

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void stamp_checksum_bytes(uint8_t *block, uint32_t block_size, int sfs2)
{
    put32(block + 4U, 0U);
    uint32_t sum = sfs2 != 0 ? 2U : 1U;
    for (uint32_t offset = 0U; offset < block_size; offset += 4U)
        sum += get32(block + offset);
    put32(block + 4U, 0U - sum);
}

static void stamp_checksum(uint8_t *block, int sfs2)
{
    stamp_checksum_bytes(block, TEST_BLOCK_SIZE, sfs2);
}

static void set_header(uint8_t *block, const char id[4], uint32_t own_block)
{
    memcpy(block, id, 4U);
    put32(block + 8U, own_block);
}

static void make_root(uint8_t *block, uint32_t own_block, uint16_t sequence,
                      int sfs2)
{
    memset(block, 0, TEST_BLOCK_SIZE);
    set_header(block, sfs2 != 0 ? "SFS\2" : "SFS\0", own_block);
    put16(block + 12U, sfs2 != 0 ? 4U : 3U);
    put16(block + 14U, sequence);
    put32(block + 48U, TEST_BLOCKS);
    put32(block + 52U, TEST_BLOCK_SIZE);
    put32(block + 96U, 1U);  /* bitmap */
    put32(block + 100U, 2U); /* admin space */
    put32(block + 104U, 4U); /* root object container */
    put32(block + 108U, 3U); /* extent B-tree root */
    put32(block + 112U, 5U); /* object node root */
    stamp_checksum(block, sfs2);
}

static void bitmap_mark_used(uint8_t *block, uint32_t number)
{
    block[12U + number / 8U] &=
        (uint8_t)~(uint8_t)(0x80U >> (number & 7U));
}

static void make_bitmap(uint8_t *block, int fragmented, int sfs2)
{
    memset(block, 0, TEST_BLOCK_SIZE);
    set_header(block, "BTMP", 1U);
    for (uint32_t number = 16U; number < 100U; ++number)
        block[12U + number / 8U] |= (uint8_t)(0x80U >> (number & 7U));
    bitmap_mark_used(block, 20U);
    if (fragmented != 0) {
        bitmap_mark_used(block, 30U);
        bitmap_mark_used(block, 31U);
    } else {
        bitmap_mark_used(block, 21U);
        bitmap_mark_used(block, 22U);
    }
    stamp_checksum(block, sfs2);
}

static void make_extent_tree(uint8_t *block, int fragmented, int sfs2)
{
    memset(block, 0, TEST_BLOCK_SIZE);
    set_header(block, "BNDC", 3U);
    put16(block + 12U, 2U);
    block[14U] = 1U;
    const uint32_t stride = sfs2 != 0 ? 16U : 14U;
    block[15U] = (uint8_t)stride;
    const uint32_t second = fragmented != 0 ? 30U : 21U;
    uint8_t *first_node = block + 16U;
    uint8_t *second_node = first_node + stride;
    put32(first_node, 20U);
    put32(first_node + 4U, second);
    put32(first_node + 8U, 0U);
    if (sfs2 != 0) put32(first_node + 12U, 1U);
    else put16(first_node + 12U, 1U);
    put32(second_node, second);
    put32(second_node + 4U, 0U);
    put32(second_node + 8U, 20U);
    if (sfs2 != 0) put32(second_node + 12U, 2U);
    else put16(second_node + 12U, 2U);
    stamp_checksum(block, sfs2);
}

static void make_object_container(uint8_t *block, int sfs2)
{
    memset(block, 0, TEST_BLOCK_SIZE);
    set_header(block, "OBJC", 4U);
    uint8_t *object = block + 24U;
    const uint64_t file_size = 3U * TEST_BLOCK_SIZE;
    put32(object + 4U, 10U);
    put32(object + 8U, 0x0fU);
    put32(object + 12U, 20U);
    if (sfs2 != 0) {
        put32(object + 16U, (uint32_t)(file_size >> 16U));
        put16(object + 20U, (uint16_t)file_size);
        object[26U] = 0U;
        memcpy(object + 27U, "frag", 5U);
        object[32U] = 0U;
    } else {
        put32(object + 16U, (uint32_t)file_size);
        object[24U] = 0U;
        memcpy(object + 25U, "frag", 5U);
        object[30U] = 0U;
    }
    stamp_checksum(block, sfs2);
}

static void make_image(uint8_t *image, int transaction_pending, int fragmented,
                       int sfs2)
{
    memset(image, 0, TEST_BYTES);
    make_root(image, 0U, 5U, sfs2);
    make_root(image + (TEST_BLOCKS - 1U) * TEST_BLOCK_SIZE,
              TEST_BLOCKS - 1U, 6U, sfs2);
    make_bitmap(image + TEST_BLOCK_SIZE, fragmented, sfs2);
    make_extent_tree(image + 3U * TEST_BLOCK_SIZE, fragmented, sfs2);
    make_object_container(image + 4U * TEST_BLOCK_SIZE, sfs2);
    memset(image + 20U * TEST_BLOCK_SIZE, 'A', TEST_BLOCK_SIZE);
    if (fragmented != 0) {
        memset(image + 30U * TEST_BLOCK_SIZE, 'B', TEST_BLOCK_SIZE);
        memset(image + 31U * TEST_BLOCK_SIZE, 'C', TEST_BLOCK_SIZE);
    } else {
        memset(image + 21U * TEST_BLOCK_SIZE, 'B', TEST_BLOCK_SIZE);
        memset(image + 22U * TEST_BLOCK_SIZE, 'C', TEST_BLOCK_SIZE);
    }
    if (transaction_pending != 0) {
        uint8_t *marker = image + 6U * TEST_BLOCK_SIZE;
        set_header(marker, "TRFA", 6U);
        stamp_checksum(marker, sfs2);
    }
}

static int save_image(const uint8_t *image, char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-sfs-XXXXXX");
    const int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    const ssize_t written = write(fd, image, TEST_BYTES);
    const int close_result = close(fd);
    if (written != (ssize_t)TEST_BYTES || close_result != 0) {
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static int analyse_image(const uint8_t *image, SfsAnalysis *analysis,
                         SfsMapCell *cells, uint64_t cell_count,
                         char *error, size_t error_size)
{
    char path[64];
    if (save_image(image, path) != 0)
        return -2;
    const int result = sfs_analyse(path, analysis, cells, cell_count,
                                   error, error_size);
    (void)unlink(path);
    return result;
}

static int make_stage_path(char path[64])
{
    (void)snprintf(path, 64U, "/tmp/linux-defragger-sfs-stage-XXXXXX");
    const int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    if (close(fd) != 0) {
        (void)unlink(path);
        return -1;
    }
    return unlink(path);
}

static int probe_image(const uint8_t *image)
{
    char path[64];
    if (save_image(image, path) != 0)
        return -1;
    const int result = sfs_probe(path) ? 1 : 0;
    (void)unlink(path);
    return result;
}


static int write_block_at(int fd, const uint8_t *block, uint32_t block_size,
                          uint32_t block_number)
{
    const off_t offset = (off_t)((uint64_t)block_number * block_size);
    return pwrite(fd, block, block_size, offset) == (ssize_t)block_size ? 0 : -1;
}

static void bitmap_set(uint8_t *block, uint32_t number, int is_free)
{
    const uint8_t mask = (uint8_t)(0x80U >> (number & 7U));
    uint8_t *slot = block + 12U + number / 8U;
    if (is_free != 0)
        *slot |= mask;
    else
        *slot &= (uint8_t)~mask;
}

/*
 * SFS2's two material on-disk differences are qualified independently here:
 * 48-bit file sizes in object records and 32-bit extent lengths.  A sparse
 * >4 GiB host file keeps the fixture cheap while forcing both fields beyond
 * the SFS0 limits; the analyser never relies on sparse-file semantics.
 */
static int test_sfs2_large_sparse(void)
{
    enum {
        LARGE_BLOCK_SIZE = 65536U,
        LARGE_BLOCKS = 70000U,
        LARGE_DATA_START = 20U,
        LARGE_DATA_BLOCKS = 65537U
    };
    const uint64_t file_size = UINT64_C(0x100000001);
    char path[] = "/tmp/linux-defragger-sfs2-large-XXXXXX";
    uint8_t *block = calloc(1U, LARGE_BLOCK_SIZE);
    if (block == NULL)
        return -1;
    int fd = mkstemp(path);
    if (fd < 0) {
        free(block);
        return -1;
    }
    const uint64_t image_bytes = (uint64_t)LARGE_BLOCKS * LARGE_BLOCK_SIZE;
    if (ftruncate(fd, (off_t)image_bytes) != 0)
        goto fail;

    memset(block, 0, LARGE_BLOCK_SIZE);
    set_header(block, "SFS\2", 0U);
    put16(block + 12U, 4U);
    put16(block + 14U, 5U);
    put32(block + 48U, LARGE_BLOCKS);
    put32(block + 52U, LARGE_BLOCK_SIZE);
    put32(block + 96U, 1U);
    put32(block + 100U, 2U);
    put32(block + 104U, 4U);
    put32(block + 108U, 3U);
    put32(block + 112U, 5U);
    stamp_checksum_bytes(block, LARGE_BLOCK_SIZE, 1);
    if (write_block_at(fd, block, LARGE_BLOCK_SIZE, 0U) != 0)
        goto fail;

    memset(block, 0, LARGE_BLOCK_SIZE);
    set_header(block, "BTMP", 1U);
    for (uint32_t number = 0U; number < LARGE_BLOCKS; ++number)
        bitmap_set(block, number, 1);
    for (uint32_t number = 0U; number < 16U; ++number)
        bitmap_set(block, number, 0);
    for (uint32_t number = LARGE_DATA_START;
         number < LARGE_DATA_START + LARGE_DATA_BLOCKS; ++number)
        bitmap_set(block, number, 0);
    bitmap_set(block, LARGE_BLOCKS - 1U, 0);
    stamp_checksum_bytes(block, LARGE_BLOCK_SIZE, 1);
    if (write_block_at(fd, block, LARGE_BLOCK_SIZE, 1U) != 0)
        goto fail;

    memset(block, 0, LARGE_BLOCK_SIZE);
    set_header(block, "BNDC", 3U);
    put16(block + 12U, 1U);
    block[14U] = 1U;
    block[15U] = 16U;
    put32(block + 16U, LARGE_DATA_START);
    put32(block + 20U, 0U);
    put32(block + 24U, 0U);
    put32(block + 28U, LARGE_DATA_BLOCKS);
    stamp_checksum_bytes(block, LARGE_BLOCK_SIZE, 1);
    if (write_block_at(fd, block, LARGE_BLOCK_SIZE, 3U) != 0)
        goto fail;

    memset(block, 0, LARGE_BLOCK_SIZE);
    set_header(block, "OBJC", 4U);
    uint8_t *object = block + 24U;
    put32(object + 4U, 10U);
    put32(object + 8U, 0x0fU);
    put32(object + 12U, LARGE_DATA_START);
    put32(object + 16U, (uint32_t)(file_size >> 16U));
    put16(object + 20U, (uint16_t)file_size);
    object[26U] = 0U;
    memcpy(object + 27U, "large", 6U);
    object[33U] = 0U;
    stamp_checksum_bytes(block, LARGE_BLOCK_SIZE, 1);
    if (write_block_at(fd, block, LARGE_BLOCK_SIZE, 4U) != 0)
        goto fail;

    memset(block, 0, LARGE_BLOCK_SIZE);
    set_header(block, "SFS\2", LARGE_BLOCKS - 1U);
    put16(block + 12U, 4U);
    put16(block + 14U, 6U);
    put32(block + 48U, LARGE_BLOCKS);
    put32(block + 52U, LARGE_BLOCK_SIZE);
    put32(block + 96U, 1U);
    put32(block + 100U, 2U);
    put32(block + 104U, 4U);
    put32(block + 108U, 3U);
    put32(block + 112U, 5U);
    stamp_checksum_bytes(block, LARGE_BLOCK_SIZE, 1);
    if (write_block_at(fd, block, LARGE_BLOCK_SIZE, LARGE_BLOCKS - 1U) != 0 ||
        fsync(fd) != 0)
        goto fail;
    if (close(fd) != 0) {
        fd = -1;
        goto fail_closed;
    }
    fd = -1;

    SfsAnalysis analysis;
    char error[256] = {0};
    const int analysed = sfs_analyse(path, &analysis, NULL, 0U,
                                     error, sizeof(error));
    if (analysed != 0 || analysis.structure_version != 4U ||
        analysis.regular_files != 1U ||
        analysis.data_blocks != LARGE_DATA_BLOCKS ||
        analysis.fragmented_files != 0U ||
        analysis.growth_10_satisfied || !sfs_probe(path)) {
        (void)fprintf(stderr, "large sparse SFS2 fixture rejected: %s\n", error);
        goto fail_closed;
    }

    free(block);
    return unlink(path) == 0 ? 0 : -1;

fail:
    (void)close(fd);
    fd = -1;
fail_closed:
    if (fd >= 0)
        (void)close(fd);
    free(block);
    (void)unlink(path);
    return -1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--write-fixture") == 0) {
        uint8_t *fixture = malloc(TEST_BYTES);
        if (fixture == NULL)
            return 2;
        const int sfs2 = strncmp(argv[3], "sfs2-", 5U) == 0 ? 1 : 0;
        const char *layout = sfs2 != 0 ? argv[3] + 5U : argv[3];
        const int fragmented = strcmp(layout, "fragmented") == 0 ? 1 :
                               strcmp(layout, "contiguous") == 0 ? 0 : -1;
        if (fragmented < 0) {
            free(fixture);
            return 2;
        }
        make_image(fixture, 0, fragmented, sfs2);
        const int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        const ssize_t written = fd >= 0 ? write(fd, fixture, TEST_BYTES) : -1;
        if (fd >= 0) (void)close(fd);
        free(fixture);
        return written == TEST_BYTES ? 0 : 1;
    }
    if (argc != 1)
        return 2;
    uint8_t *image = malloc(TEST_BYTES);
    if (image == NULL)
        return 1;

    SfsAnalysis analysis;
    SfsMapCell cells[16];
    char error[256] = {0};

    make_image(image, 0, 1, 0);
    if (analyse_image(image, &analysis, cells, 16U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "valid SFS image rejected: %s\n", error);
        free(image);
        return 2;
    }
    if (analysis.block_size != TEST_BLOCK_SIZE ||
        analysis.total_blocks != TEST_BLOCKS ||
        analysis.free_blocks != 81U || analysis.used_blocks != 47U ||
        analysis.data_blocks != 3U || analysis.regular_files != 1U ||
        analysis.fragmented_files != 1U || analysis.growth_10_satisfied ||
        analysis.sequence_number != 6U ||
        !analysis.primary_root_valid || !analysis.backup_root_valid ||
        analysis.transaction_pending) {
        (void)fprintf(stderr, "unexpected SFS analysis totals or root selection\n");
        free(image);
        return 3;
    }
    uint64_t mapped_free = 0U;
    uint64_t mapped_used = 0U;
    for (size_t index = 0U; index < 16U; ++index) {
        mapped_free += cells[index].free_count;
        mapped_used += cells[index].used_count;
    }
    uint64_t mapped_fragmented = 0U;
    for (size_t index = 0U; index < 16U; ++index)
        mapped_fragmented += cells[index].fragmented_count;
    if (mapped_free != analysis.free_blocks || mapped_used != analysis.used_blocks ||
        mapped_fragmented != analysis.data_blocks) {
        (void)fprintf(stderr, "SFS cell accounting disagrees with catalogue totals\n");
        free(image);
        return 4;
    }

    make_image(image, 0, 0, 0);
    if (analyse_image(image, &analysis, cells, 16U, error, sizeof(error)) != 0 ||
        analysis.fragmented_files != 0U || !analysis.growth_10_satisfied) {
        (void)fprintf(stderr, "contiguous SFS extent chain was not recognised: %s\n", error);
        free(image);
        return 5;
    }

    make_image(image, 0, 1, 1);
    if (analyse_image(image, &analysis, cells, 16U, error, sizeof(error)) != 0 ||
        analysis.structure_version != 4U ||
        analysis.fragmented_files != 1U ||
        analysis.data_blocks != 3U) {
        (void)fprintf(stderr, "valid SFS2 image rejected: %s\n", error);
        free(image);
        return 15;
    }

    make_image(image, 0, 1, 1);
    stamp_checksum(image + TEST_BLOCK_SIZE, 0);
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) == 0) {
        (void)fprintf(stderr,
                      "SFS2 metadata using the SFS0 checksum convention was accepted\n");
        free(image);
        return 20;
    }

    make_image(image, 0, 1, 0);
    {
        uint8_t *const object_block = image + 4U * TEST_BLOCK_SIZE;
        uint8_t *const name = object_block + 24U + 25U;
        memset(name, 'A', 108U);
        name[108U] = 0U;
        name[109U] = 0U;
        stamp_checksum(object_block, 0);
    }
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) == 0) {
        (void)fprintf(stderr, "overlength SFS object name was accepted\n");
        free(image);
        return 21;
    }

    char sfs2_source[64];
    char sfs2_stage[64];
    uint64_t sfs2_commit_bytes = 0U;
    if (save_image(image, sfs2_source) != 0 ||
        make_stage_path(sfs2_stage) != 0 ||
        sfs_build_stage(sfs2_source, sfs2_stage, false, 10U, false,
                        &sfs2_commit_bytes, error, sizeof(error)) != 0 ||
        sfs2_commit_bytes != TEST_BYTES ||
        sfs_verify_layout(sfs2_stage, false, 10U,
                          error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "SFS2 Defrag stage failed: %s\n", error);
        (void)unlink(sfs2_source);
        (void)unlink(sfs2_stage);
        free(image);
        return 16;
    }

    SfsAnalysis sfs2_staged;
    if (sfs_analyse(sfs2_stage, &sfs2_staged, NULL, 0U,
                    error, sizeof(error)) != 0 ||
        sfs2_staged.structure_version != 4U ||
        sfs2_staged.fragmented_files != 0U) {
        (void)fprintf(stderr, "SFS2 Defrag stage did not verify: %s\n", error);
        (void)unlink(sfs2_source);
        (void)unlink(sfs2_stage);
        free(image);
        return 17;
    }

    (void)unlink(sfs2_stage);
    if (make_stage_path(sfs2_stage) != 0 ||
        sfs_build_stage(sfs2_source, sfs2_stage, true, 10U, false,
                        &sfs2_commit_bytes, error, sizeof(error)) != 0 ||
        sfs_verify_layout(sfs2_stage, true, 10U,
                          error, sizeof(error)) != 0 ||
        sfs_analyse(sfs2_stage, &sfs2_staged, NULL, 0U,
                    error, sizeof(error)) != 0 ||
        !sfs2_staged.growth_10_satisfied) {
        (void)fprintf(stderr, "SFS2 Growth Defrag stage failed: %s\n", error);
        (void)unlink(sfs2_source);
        (void)unlink(sfs2_stage);
        free(image);
        return 18;
    }
    (void)unlink(sfs2_source);
    (void)unlink(sfs2_stage);

    if (test_sfs2_large_sparse() != 0) {
        (void)fprintf(stderr, "SFS2 large-file qualification failed\n");
        free(image);
        return 19;
    }

    make_image(image, 1, 1, 0);
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) != 0 ||
        !analysis.transaction_pending) {
        (void)fprintf(stderr, "SFS unfinished transaction was not reported: %s\n", error);
        free(image);
        return 5;
    }

    make_image(image, 0, 1, 0);
    image[TEST_BLOCK_SIZE + 20U] ^= 1U;
    if (probe_image(image) != 1) {
        (void)fprintf(stderr, "SFS identity probe depended on bitmap health\n");
        free(image);
        return 6;
    }
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) == 0) {
        (void)fprintf(stderr, "corrupt SFS bitmap checksum was accepted\n");
        free(image);
        return 7;
    }

    make_image(image, 0, 1, 0);
    image[4U] ^= 1U;
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) != 0 ||
        analysis.primary_root_valid || !analysis.backup_root_valid ||
        analysis.sequence_number != 6U) {
        (void)fprintf(stderr, "SFS backup root recovery failed: %s\n", error);
        free(image);
        return 8;
    }

    make_image(image, 0, 1, 0);
    put32(image + (TEST_BLOCKS - 1U) * TEST_BLOCK_SIZE + 48U, TEST_BLOCKS - 1U);
    stamp_checksum(image + (TEST_BLOCKS - 1U) * TEST_BLOCK_SIZE, 0);
    if (analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) == 0) {
        (void)fprintf(stderr, "disagreeing SFS redundant-root geometry was accepted\n");
        free(image);
        return 9;
    }

    make_image(image, 0, 1, 0);
    char source[64];
    char stage[64];
    if (save_image(image, source) != 0 || make_stage_path(stage) != 0) {
        (void)fprintf(stderr, "cannot create SFS relocation test paths\n");
        free(image);
        return 10;
    }
    uint64_t commit_bytes = 0U;
    if (sfs_build_stage(source, stage, false, 10U, false,
                        &commit_bytes, error, sizeof(error)) != 0 ||
        commit_bytes != TEST_BYTES ||
        sfs_verify_layout(stage, false, 10U, error, sizeof(error)) != 0 ||
        analyse_image(image, &analysis, NULL, 0U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "SFS Defrag stage failed: %s\n", error);
        (void)unlink(source);
        (void)unlink(stage);
        free(image);
        return 11;
    }
    uint8_t payload[3U * TEST_BLOCK_SIZE];
    const int stage_fd = open(stage, O_RDONLY | O_CLOEXEC);
    const ssize_t payload_read = stage_fd >= 0
        ? pread(stage_fd, payload, sizeof(payload), 16U * TEST_BLOCK_SIZE) : -1;
    if (stage_fd >= 0) (void)close(stage_fd);
    if (payload_read != (ssize_t)sizeof(payload) ||
        payload[0] != 'A' || payload[TEST_BLOCK_SIZE] != 'B' ||
        payload[2U * TEST_BLOCK_SIZE] != 'C') {
        (void)fprintf(stderr, "SFS Defrag did not preserve logical payload order\n");
        (void)unlink(source);
        (void)unlink(stage);
        free(image);
        return 12;
    }
    SfsAnalysis staged;
    if (sfs_analyse(stage, &staged, NULL, 0U, error, sizeof(error)) != 0 ||
        staged.fragmented_files != 0U || staged.data_blocks != 3U) {
        (void)fprintf(stderr, "SFS Defrag stage did not become contiguous: %s\n", error);
        (void)unlink(source);
        (void)unlink(stage);
        free(image);
        return 13;
    }
    (void)unlink(stage);
    if (make_stage_path(stage) != 0 ||
        sfs_build_stage(source, stage, true, 10U, false,
                        &commit_bytes, error, sizeof(error)) != 0 ||
        sfs_verify_layout(stage, true, 10U, error, sizeof(error)) != 0 ||
        sfs_analyse(stage, &staged, NULL, 0U, error, sizeof(error)) != 0 ||
        !staged.growth_10_satisfied) {
        (void)fprintf(stderr, "SFS Growth Defrag stage failed: %s\n", error);
        (void)unlink(source);
        (void)unlink(stage);
        free(image);
        return 14;
    }
    (void)unlink(source);
    (void)unlink(stage);

    free(image);
    return 0;
}
