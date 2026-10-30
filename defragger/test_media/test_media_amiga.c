// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <infiltratr/endian.h>
#include <infiltratr/posix_io.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AMIGA_BLOCK_SIZE 512U
#define AMIGA_LONGS 128U
#define AMIGA_HASH_SIZE 72U
#define AMIGA_ROOT_TAIL_LONG 78U
#define AMIGA_ROOT_BMAPS 25U
#define AMIGA_BITMAP_WORDS 127U
#define AMIGA_BITMAP_BITS (AMIGA_BITMAP_WORDS * 32U)


static uint32_t block_word(const unsigned char block[AMIGA_BLOCK_SIZE], uint32_t index) {
    return infiltratr_load_be32(block + (size_t)index * 4U);
}

static void set_block_word(unsigned char block[AMIGA_BLOCK_SIZE], uint32_t index, uint32_t value) {
    infiltratr_store_be32(block + (size_t)index * 4U, value);
}

static uint32_t checksum_value(const unsigned char block[AMIGA_BLOCK_SIZE], uint32_t checksum_index) {
    uint32_t sum = 0U;
    uint32_t index;
    for (index = 0U; index < AMIGA_LONGS; ++index) {
        if (index != checksum_index) sum += block_word(block, index);
    }
    return (uint32_t)(0U - sum);
}

static int write_block(int fd, uint32_t block_number, const unsigned char block[AMIGA_BLOCK_SIZE]) {
    const uint64_t offset = (uint64_t)block_number * AMIGA_BLOCK_SIZE;
    return infiltratr_pwrite_full(fd, block, AMIGA_BLOCK_SIZE, offset);
}

static int read_block(int fd, uint32_t block_number, unsigned char block[AMIGA_BLOCK_SIZE]) {
    const uint64_t offset = (uint64_t)block_number * AMIGA_BLOCK_SIZE;
    return infiltratr_pread_full(fd, block, AMIGA_BLOCK_SIZE, offset);
}

static uint32_t next_metadata_block(uint32_t *cursor, uint32_t root) {
    uint32_t value = *cursor;
    if (value == root) ++value;
    *cursor = value + 1U;
    if (*cursor == root) ++(*cursor);
    return value;
}

static void set_disk_name(unsigned char root[AMIGA_BLOCK_SIZE], const char *label) {
    unsigned char *name = root + (size_t)AMIGA_ROOT_TAIL_LONG * 4U + 120U;
    size_t length = label != NULL ? strlen(label) : 0U;
    if (length > 30U) length = 30U;
    name[0] = (unsigned char)length;
    if (length > 0U) memcpy(name + 1U, label, length);
}

int ldtm_format_amiga_volume(const char *path, uint8_t dostype, const char *label) {
    int fd = -1;
    off_t bytes;
    uint64_t blocks64;
    uint32_t blocks;
    uint32_t root;
    uint32_t bitmap_count;
    uint32_t extension_count;
    uint32_t *bitmap_blocks = NULL;
    uint32_t *extension_blocks = NULL;
    unsigned char *used = NULL;
    uint32_t cursor = 2U;
    uint32_t index;
    int result = -1;

    if (path == NULL || (dostype != 0U && dostype != 1U)) return -1;
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    bytes = lseek(fd, 0, SEEK_END);
    if (bytes <= 0 || ((uint64_t)bytes % AMIGA_BLOCK_SIZE) != 0U) goto cleanup;
    blocks64 = (uint64_t)bytes / AMIGA_BLOCK_SIZE;
    if (blocks64 < 1024U || blocks64 > UINT32_MAX) goto cleanup;
    blocks = (uint32_t)blocks64;
    root = blocks / 2U;
    bitmap_count = (uint32_t)(((uint64_t)blocks - 2U + AMIGA_BITMAP_BITS - 1U) / AMIGA_BITMAP_BITS);
    extension_count = bitmap_count > AMIGA_ROOT_BMAPS
        ? (bitmap_count - AMIGA_ROOT_BMAPS + AMIGA_BITMAP_WORDS - 1U) / AMIGA_BITMAP_WORDS
        : 0U;

    bitmap_blocks = calloc(bitmap_count, sizeof(*bitmap_blocks));
    extension_blocks = calloc(extension_count > 0U ? extension_count : 1U, sizeof(*extension_blocks));
    used = calloc(blocks, 1U);
    if (bitmap_blocks == NULL || extension_blocks == NULL || used == NULL) goto cleanup;
    used[0] = 1U;
    used[1] = 1U;
    used[root] = 1U;

    for (index = 0U; index < bitmap_count; ++index) {
        bitmap_blocks[index] = next_metadata_block(&cursor, root);
        if (bitmap_blocks[index] >= blocks) goto cleanup;
        used[bitmap_blocks[index]] = 1U;
    }
    for (index = 0U; index < extension_count; ++index) {
        extension_blocks[index] = next_metadata_block(&cursor, root);
        if (extension_blocks[index] >= blocks) goto cleanup;
        used[extension_blocks[index]] = 1U;
    }

    {
        unsigned char zero[AMIGA_BLOCK_SIZE] = {0};
        unsigned char boot[AMIGA_BLOCK_SIZE] = {0};
        memcpy(boot, "DOS", 3U);
        boot[3] = dostype;
        if (write_block(fd, 0U, boot) != 0 || write_block(fd, 1U, zero) != 0) goto cleanup;
    }

    for (index = 0U; index < bitmap_count; ++index) {
        unsigned char block[AMIGA_BLOCK_SIZE] = {0};
        uint32_t word_index;
        for (word_index = 0U; word_index < AMIGA_BITMAP_WORDS; ++word_index) {
            uint32_t word = 0U;
            uint32_t bit_index;
            for (bit_index = 0U; bit_index < 32U; ++bit_index) {
                const uint64_t map_bit = (uint64_t)index * AMIGA_BITMAP_BITS +
                                         (uint64_t)word_index * 32U + bit_index;
                const uint64_t block_number = 2U + map_bit;
                if (block_number < blocks && used[block_number] == 0U) word |= UINT32_C(1) << bit_index;
            }
            set_block_word(block, 1U + word_index, word);
        }
        set_block_word(block, 0U, checksum_value(block, 0U));
        if (write_block(fd, bitmap_blocks[index], block) != 0) goto cleanup;
    }

    for (index = 0U; index < extension_count; ++index) {
        unsigned char block[AMIGA_BLOCK_SIZE] = {0};
        uint32_t pointer_index;
        const uint32_t start = AMIGA_ROOT_BMAPS + index * AMIGA_BITMAP_WORDS;
        for (pointer_index = 0U; pointer_index < AMIGA_BITMAP_WORDS; ++pointer_index) {
            const uint32_t source = start + pointer_index;
            if (source >= bitmap_count) break;
            set_block_word(block, pointer_index, bitmap_blocks[source]);
        }
        set_block_word(block, 127U, index + 1U < extension_count ? extension_blocks[index + 1U] : 0U);
        if (write_block(fd, extension_blocks[index], block) != 0) goto cleanup;
    }

    {
        unsigned char root_block[AMIGA_BLOCK_SIZE] = {0};
        set_block_word(root_block, 0U, 2U);
        set_block_word(root_block, 3U, AMIGA_HASH_SIZE);
        set_block_word(root_block, AMIGA_ROOT_TAIL_LONG, UINT32_MAX);
        for (index = 0U; index < bitmap_count && index < AMIGA_ROOT_BMAPS; ++index) {
            set_block_word(root_block, AMIGA_ROOT_TAIL_LONG + 1U + index, bitmap_blocks[index]);
        }
        set_block_word(root_block, AMIGA_ROOT_TAIL_LONG + 26U,
                       extension_count > 0U ? extension_blocks[0] : 0U);
        set_disk_name(root_block, label);
        set_block_word(root_block, 127U, 1U);
        set_block_word(root_block, 5U, checksum_value(root_block, 5U));
        if (write_block(fd, root, root_block) != 0) goto cleanup;
    }

    if (fsync(fd) != 0) goto cleanup;
    result = 0;
cleanup:
    free(used);
    free(extension_blocks);
    free(bitmap_blocks);
    if (fd >= 0) (void)close(fd);
    return result;
}

int ldtm_validate_amiga_volume(const char *path, uint8_t expected_dostype) {
    /* FFS validation is owned by FFS.so. Do not run this historical
     * raw Amiga formatter's private FFS validation path. */
    if(expected_dostype==1U) {
        extern int ldtm_validate_ffs_module(const char *);
        return ldtm_validate_ffs_module(path);
    }

    int fd = -1;
    off_t bytes;
    uint64_t blocks64;
    uint32_t blocks;
    uint32_t root;
    uint32_t bitmap_count;
    uint32_t found = 0U;
    uint32_t index;
    uint32_t extension;
    unsigned char block[AMIGA_BLOCK_SIZE];
    int result = -1;

    if (path == NULL || (expected_dostype != 0U && expected_dostype != 1U)) return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    bytes = lseek(fd, 0, SEEK_END);
    if (bytes <= 0 || ((uint64_t)bytes % AMIGA_BLOCK_SIZE) != 0U) goto cleanup;
    blocks64 = (uint64_t)bytes / AMIGA_BLOCK_SIZE;
    if (blocks64 < 1024U || blocks64 > UINT32_MAX) goto cleanup;
    blocks = (uint32_t)blocks64;
    root = blocks / 2U;
    bitmap_count = (uint32_t)(((uint64_t)blocks - 2U + AMIGA_BITMAP_BITS - 1U) / AMIGA_BITMAP_BITS);

    if (read_block(fd, 0U, block) != 0 || memcmp(block, "DOS", 3U) != 0 || block[3] != expected_dostype) goto cleanup;
    if (read_block(fd, root, block) != 0 || block_word(block, 0U) != 2U ||
        block_word(block, 3U) != AMIGA_HASH_SIZE || block_word(block, 127U) != 1U) goto cleanup;
    {
        uint32_t sum = 0U;
        for (index = 0U; index < AMIGA_LONGS; ++index) sum += block_word(block, index);
        if (sum != 0U || block_word(block, AMIGA_ROOT_TAIL_LONG) != UINT32_MAX) goto cleanup;
    }
    for (index = 0U; index < AMIGA_ROOT_BMAPS && found < bitmap_count; ++index) {
        const uint32_t bitmap = block_word(block, AMIGA_ROOT_TAIL_LONG + 1U + index);
        unsigned char bitmap_data[AMIGA_BLOCK_SIZE];
        uint32_t sum = 0U;
        uint32_t word;
        if (bitmap == 0U || bitmap >= blocks || read_block(fd, bitmap, bitmap_data) != 0) goto cleanup;
        for (word = 0U; word < AMIGA_LONGS; ++word) sum += block_word(bitmap_data, word);
        if (sum != 0U) goto cleanup;
        ++found;
    }
    extension = block_word(block, AMIGA_ROOT_TAIL_LONG + 26U);
    while (found < bitmap_count) {
        unsigned char extension_data[AMIGA_BLOCK_SIZE];
        if (extension == 0U || extension >= blocks || read_block(fd, extension, extension_data) != 0) goto cleanup;
        for (index = 0U; index < AMIGA_BITMAP_WORDS && found < bitmap_count; ++index) {
            const uint32_t bitmap = block_word(extension_data, index);
            unsigned char bitmap_data[AMIGA_BLOCK_SIZE];
            uint32_t sum = 0U;
            uint32_t word;
            if (bitmap == 0U || bitmap >= blocks || read_block(fd, bitmap, bitmap_data) != 0) goto cleanup;
            for (word = 0U; word < AMIGA_LONGS; ++word) sum += block_word(bitmap_data, word);
            if (sum != 0U) goto cleanup;
            ++found;
        }
        extension = block_word(extension_data, 127U);
    }
    result = 0;
cleanup:
    if (fd >= 0) (void)close(fd);
    return result;
}
