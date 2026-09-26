// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "affs_native.h"
#include "ld_io.h"

#include "infiltratr/arithmetic.h"
#include "infiltratr/core.h"
#include "infiltratr/endian.h"
#include "infiltratr/posix_io.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AMIGA_BLOCK_SIZE 512U
#define AMIGA_LONGS 128U
#define AMIGA_HASH_SIZE 72U
#define AMIGA_NAME_OFFSET 432U
#define AMIGA_TYPE_SHORT 2U
#define AMIGA_TYPE_DATA 8U
#define AMIGA_TYPE_LIST 16U
#define AMIGA_SUBTYPE_DIR 2U
#define AMIGA_SUBTYPE_FILE UINT32_C(0xfffffffd)
#define AMIGA_HASH_CHAIN_LONG 124U
#define AMIGA_PARENT_LONG 125U
#define AMIGA_EXTENSION_LONG 126U
#define AMIGA_SUBTYPE_LONG 127U
#define AMIGA_BYTE_SIZE_LONG 81U
#define AMIGA_DATA_POINTER_LONG 77U
#define AMIGA_MAX_TARGET_FILES 8U

typedef struct {
    uint32_t *values;
    size_t count;
    size_t capacity;
} AmigaBlockVec;

typedef struct {
    uint32_t header;
    char name[32];
    AmigaBlockVec data;
    AmigaBlockVec lists;
} AmigaTarget;

static uint32_t get_be32(const unsigned char *p) {
    return infiltratr_load_be32(p);
}

static void put_be32(unsigned char *p, uint32_t value) {
    infiltratr_store_be32(p, value);
}

static uint32_t block_word(const unsigned char block[AMIGA_BLOCK_SIZE], uint32_t index) {
    return get_be32(block + (size_t)index * 4U);
}

static void set_block_word(unsigned char block[AMIGA_BLOCK_SIZE], uint32_t index, uint32_t value) {
    put_be32(block + (size_t)index * 4U, value);
}

static uint32_t checksum_value(const unsigned char block[AMIGA_BLOCK_SIZE], uint32_t checksum_index) {
    uint32_t sum = 0U;
    uint32_t index;
    for (index = 0U; index < AMIGA_LONGS; ++index) {
        if (index != checksum_index) sum += block_word(block, index);
    }
    return (uint32_t)(0U - sum);
}

static int checksum_ok(const unsigned char block[AMIGA_BLOCK_SIZE]) {
    uint32_t sum = 0U;
    uint32_t index;
    for (index = 0U; index < AMIGA_LONGS; ++index) sum += block_word(block, index);
    return sum == 0U;
}

static void fix_checksum(unsigned char block[AMIGA_BLOCK_SIZE], uint32_t checksum_index) {
    set_block_word(block, checksum_index, 0U);
    set_block_word(block, checksum_index, checksum_value(block, checksum_index));
}

static int write_block(int fd, uint32_t block_number, const unsigned char block[AMIGA_BLOCK_SIZE]) {
    const uint64_t offset = (uint64_t)block_number * AMIGA_BLOCK_SIZE;
    return infiltratr_pwrite_full(fd, block, AMIGA_BLOCK_SIZE, offset);
}

static int read_block(int fd, uint32_t block_number, unsigned char block[AMIGA_BLOCK_SIZE]) {
    const uint64_t offset = (uint64_t)block_number * AMIGA_BLOCK_SIZE;
    return infiltratr_pread_full(fd, block, AMIGA_BLOCK_SIZE, offset);
}

static void set_bstr_name(unsigned char block[AMIGA_BLOCK_SIZE], const char *name) {
    unsigned char *target = block + AMIGA_NAME_OFFSET;
    size_t length = name != NULL ? strlen(name) : 0U;
    if (length > 30U) length = 30U;
    memset(target, 0, 31U);
    target[0] = (unsigned char)length;
    if (length > 0U) memcpy(target + 1U, name, length);
}

static void get_bstr_name(const unsigned char block[AMIGA_BLOCK_SIZE], char name[32]) {
    size_t length = block[AMIGA_NAME_OFFSET];
    if (length > 30U) length = 30U;
    memcpy(name, block + AMIGA_NAME_OFFSET + 1U, length);
    name[length] = '\0';
}

static uint32_t amiga_name_hash(const char *name) {
    const size_t length = strlen(name);
    uint32_t hash = (uint32_t)length;
    size_t index;
    for (index = 0U; index < length; ++index) {
        hash = (hash * 13U + (uint32_t)toupper((unsigned char)name[index])) & UINT32_C(0x7ff);
    }
    return hash % AMIGA_HASH_SIZE;
}

static void block_vec_free(AmigaBlockVec *vec) {
    if (vec == NULL) return;
    free(vec->values);
    memset(vec, 0, sizeof(*vec));
}

static int block_vec_push(AmigaBlockVec *vec, uint32_t value) {
    if (vec->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&vec->values, &vec->capacity,
                                  sizeof(*vec->values), vec->count + 1U, 128U))
        return -1;
    vec->values[vec->count++] = value;
    return 0;
}

static int allocate_one(AffsVolume *volume, uint32_t *cursor, uint32_t *block_number) {
    uint32_t candidate;
    uint32_t start;
    if (volume == NULL || cursor == NULL || block_number == NULL || volume->blocks <= 2U) return -1;
    start = *cursor < 2U || *cursor >= volume->blocks ? 2U : *cursor;
    candidate = start;
    do {
        if (ld_bitmap_get(volume->free_map, candidate)) {
            ld_bitmap_set(volume->free_map, candidate, false);
            *block_number = candidate;
            *cursor = candidate + 1U < volume->blocks ? candidate + 1U : 2U;
            return 0;
        }
        candidate = candidate + 1U < volume->blocks ? candidate + 1U : 2U;
    } while (candidate != start);
    return -1;
}

static int run_is_free(const AffsVolume *volume, uint32_t start, uint32_t count) {
    uint32_t index;
    if (count == 0U || start < 2U || start > volume->blocks || count > volume->blocks - start) return 0;
    for (index = 0U; index < count; ++index) {
        if (!ld_bitmap_get(volume->free_map, (uint64_t)start + index)) return 0;
    }
    return 1;
}

static int allocate_run(AffsVolume *volume, uint32_t *cursor, uint32_t count, AmigaBlockVec *output) {
    uint32_t candidate;
    uint32_t index;
    uint32_t start;
    if (volume == NULL || cursor == NULL || output == NULL || count == 0U) return -1;
    start = *cursor < 2U || *cursor >= volume->blocks ? 2U : *cursor;
    for (candidate = start; candidate <= volume->blocks - count; ++candidate) {
        if (!run_is_free(volume, candidate, count)) continue;
        for (index = 0U; index < count; ++index) {
            ld_bitmap_set(volume->free_map, (uint64_t)candidate + index, false);
            if (block_vec_push(output, candidate + index) != 0) return -1;
        }
        *cursor = candidate + count < volume->blocks ? candidate + count : 2U;
        return 0;
    }
    for (candidate = 2U; candidate < start && candidate <= volume->blocks - count; ++candidate) {
        if (!run_is_free(volume, candidate, count)) continue;
        for (index = 0U; index < count; ++index) {
            ld_bitmap_set(volume->free_map, (uint64_t)candidate + index, false);
            if (block_vec_push(output, candidate + index) != 0) return -1;
        }
        *cursor = candidate + count < volume->blocks ? candidate + count : 2U;
        return 0;
    }
    return -1;
}

static void deterministic_payload(unsigned char *buffer, size_t length,
                                  uint32_t file_index, uint32_t data_index) {
    uint64_t state = UINT64_C(0x6a09e667f3bcc909) ^
                     ((uint64_t)file_index << 40) ^
                     ((uint64_t)data_index * UINT64_C(0x9e3779b97f4a7c15));
    size_t offset;
    for (offset = 0U; offset < length; ++offset) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= UINT64_C(0x2545f4914f6cdd1d);
        buffer[offset] = (unsigned char)(state >> 56);
    }
}

static int rewrite_bitmap(AffsVolume *volume) {
    size_t bit = 0U;
    size_t bitmap_index;
    const size_t bit_count = (size_t)volume->blocks - 2U;
    for (bitmap_index = 0U; bitmap_index < volume->bitmap_blocks.n && bit < bit_count; ++bitmap_index) {
        unsigned char block[AMIGA_BLOCK_SIZE] = {0};
        uint32_t word_index;
        for (word_index = 1U; word_index < AMIGA_LONGS && bit < bit_count; ++word_index) {
            uint32_t word = 0U;
            uint32_t bit_index;
            for (bit_index = 0U; bit_index < 32U && bit < bit_count; ++bit_index, ++bit) {
                if (ld_bitmap_get(volume->free_map, 2U + bit)) word |= UINT32_C(1) << bit_index;
            }
            set_block_word(block, word_index, word);
        }
        fix_checksum(block, 0U);
        if (write_block(volume->fd, volume->bitmap_blocks.v[bitmap_index], block) != 0) return -1;
    }
    return bit == bit_count ? 0 : -1;
}

static void init_directory_block(unsigned char block[AMIGA_BLOCK_SIZE], uint32_t self,
                                 uint32_t parent, const char *name) {
    memset(block, 0, AMIGA_BLOCK_SIZE);
    set_block_word(block, 0U, AMIGA_TYPE_SHORT);
    set_block_word(block, 1U, self);
    set_block_word(block, 3U, AMIGA_HASH_SIZE);
    set_block_word(block, AMIGA_PARENT_LONG, parent);
    set_block_word(block, AMIGA_SUBTYPE_LONG, AMIGA_SUBTYPE_DIR);
    set_bstr_name(block, name);
}

static void init_empty_file_header(unsigned char block[AMIGA_BLOCK_SIZE], uint32_t self,
                                   uint32_t parent, const char *name) {
    memset(block, 0, AMIGA_BLOCK_SIZE);
    set_block_word(block, 0U, AMIGA_TYPE_SHORT);
    set_block_word(block, 1U, self);
    set_block_word(block, AMIGA_BYTE_SIZE_LONG, 0U);
    set_block_word(block, AMIGA_PARENT_LONG, parent);
    set_block_word(block, AMIGA_SUBTYPE_LONG, AMIGA_SUBTYPE_FILE);
    set_bstr_name(block, name);
    fix_checksum(block, 5U);
}

static int link_child(int fd, unsigned char directory[AMIGA_BLOCK_SIZE],
                      uint32_t child_block, const char *name) {
    unsigned char child[AMIGA_BLOCK_SIZE];
    const uint32_t bucket = amiga_name_hash(name);
    const uint32_t current = block_word(directory, 6U + bucket);
    if (read_block(fd, child_block, child) != 0) return -1;
    set_block_word(child, AMIGA_HASH_CHAIN_LONG, current);
    fix_checksum(child, 5U);
    if (write_block(fd, child_block, child) != 0) return -1;
    set_block_word(directory, 6U + bucket, child_block);
    return 0;
}

static int write_target_metadata(AffsVolume *volume, const AmigaTarget *target,
                                 uint32_t parent, uint32_t file_size) {
    unsigned char block[AMIGA_BLOCK_SIZE];
    size_t data_offset;
    size_t list_index;
    uint32_t count = target->data.count < AMIGA_HASH_SIZE
                         ? (uint32_t)target->data.count
                         : AMIGA_HASH_SIZE;
    memset(block, 0, sizeof(block));
    set_block_word(block, 0U, AMIGA_TYPE_SHORT);
    set_block_word(block, 1U, target->header);
    set_block_word(block, 2U, count);
    if (target->data.count > 0U) set_block_word(block, 4U, target->data.values[0]);
    for (uint32_t index = 0U; index < count; ++index) {
        set_block_word(block, AMIGA_DATA_POINTER_LONG - index, target->data.values[index]);
    }
    data_offset = count;
    set_block_word(block, AMIGA_BYTE_SIZE_LONG, file_size);
    set_block_word(block, AMIGA_PARENT_LONG, parent);
    set_block_word(block, AMIGA_EXTENSION_LONG,
                   target->lists.count > 0U ? target->lists.values[0] : 0U);
    set_block_word(block, AMIGA_SUBTYPE_LONG, AMIGA_SUBTYPE_FILE);
    set_bstr_name(block, target->name);
    fix_checksum(block, 5U);
    if (write_block(volume->fd, target->header, block) != 0) return -1;

    for (list_index = 0U; list_index < target->lists.count; ++list_index) {
        const size_t remaining = target->data.count - data_offset;
        const uint32_t list_count = remaining < AMIGA_HASH_SIZE ? (uint32_t)remaining : AMIGA_HASH_SIZE;
        memset(block, 0, sizeof(block));
        set_block_word(block, 0U, AMIGA_TYPE_LIST);
        set_block_word(block, 1U, target->lists.values[list_index]);
        set_block_word(block, 2U, list_count);
        for (uint32_t index = 0U; index < list_count; ++index) {
            set_block_word(block, AMIGA_DATA_POINTER_LONG - index,
                           target->data.values[data_offset + index]);
        }
        data_offset += list_count;
        set_block_word(block, AMIGA_PARENT_LONG, parent);
        set_block_word(block, AMIGA_EXTENSION_LONG,
                       list_index + 1U < target->lists.count ? target->lists.values[list_index + 1U] : 0U);
        set_block_word(block, AMIGA_SUBTYPE_LONG, AMIGA_SUBTYPE_FILE);
        fix_checksum(block, 5U);
        if (write_block(volume->fd, target->lists.values[list_index], block) != 0) return -1;
    }
    return data_offset == target->data.count ? 0 : -1;
}

static int write_target_data(AffsVolume *volume, const AmigaTarget *target,
                             uint8_t dostype, uint32_t file_index, uint64_t file_size) {
    const size_t payload_size = dostype == 0U ? AMIGA_BLOCK_SIZE - 24U : AMIGA_BLOCK_SIZE;
    uint64_t remaining = file_size;
    size_t data_index;
    unsigned char expected[AMIGA_BLOCK_SIZE];
    unsigned char block[AMIGA_BLOCK_SIZE];
    for (data_index = 0U; data_index < target->data.count; ++data_index) {
        const size_t take = remaining < payload_size ? (size_t)remaining : payload_size;
        deterministic_payload(expected, take, file_index, (uint32_t)data_index);
        memset(block, 0, sizeof(block));
        if (dostype == 0U) {
            set_block_word(block, 0U, AMIGA_TYPE_DATA);
            set_block_word(block, 1U, target->header);
            set_block_word(block, 2U, (uint32_t)data_index + 1U);
            set_block_word(block, 3U, (uint32_t)take);
            set_block_word(block, 4U,
                           data_index + 1U < target->data.count ? target->data.values[data_index + 1U] : 0U);
            memcpy(block + 24U, expected, take);
            fix_checksum(block, 5U);
        } else {
            memcpy(block, expected, take);
        }
        if (write_block(volume->fd, target->data.values[data_index], block) != 0) return -1;
        remaining -= take;
    }
    return remaining == 0U ? 0 : -1;
}

int ldtm_populate_amiga_volume(const char *path, uint8_t dostype,
                               const LdtmFragmentProfile *profile) {
    AffsVolume volume;
    char *error = NULL;
    AmigaTarget targets[AMIGA_MAX_TARGET_FILES];
    unsigned char root[AMIGA_BLOCK_SIZE];
    unsigned char data_dir[AMIGA_BLOCK_SIZE];
    unsigned char files_dir[AMIGA_BLOCK_SIZE];
    unsigned char fragmented_dir[AMIGA_BLOCK_SIZE];
    uint32_t data_dir_block;
    uint32_t files_dir_block;
    uint32_t fragmented_dir_block;
    uint32_t metadata_cursor = 2U;
    uint32_t data_cursor;
    uint64_t file_sizes[AMIGA_MAX_TARGET_FILES] = {0};
    uint32_t data_blocks[AMIGA_MAX_TARGET_FILES] = {0};
    uint32_t payload_size;
    size_t file_index;
    uint32_t index;
    int result = -1;

    memset(targets, 0, sizeof(targets));
    if (path == NULL || profile == NULL || (dostype != 0U && dostype != 1U) ||
        profile->files == 0U || profile->files > AMIGA_MAX_TARGET_FILES ||
        profile->chunk_kib == 0U) return -1;
    payload_size = dostype == 0U ? AMIGA_BLOCK_SIZE - 24U : AMIGA_BLOCK_SIZE;
    for (file_index = 0U; file_index < profile->files; ++file_index) {
        const uint32_t chunks = ldtm_profile_file_chunks(profile, file_index);
        const uint64_t bytes = ldtm_profile_file_bytes(profile, file_index);
        if (chunks < 2U || bytes == 0U || bytes > UINT32_MAX)
            return -1;
        file_sizes[file_index] = bytes;
        const uint64_t blocks = (bytes + payload_size - 1U) / payload_size;
        if (blocks == 0U || blocks > UINT32_MAX)
            return -1;
        data_blocks[file_index] = (uint32_t)blocks;
    }

    if (affs_scan(path, true, &volume, &error) != 0) goto cleanup_error;
    if (volume.dostype != dostype || volume.files.n != 0U || volume.directory_blocks.n != 1U) goto cleanup_volume;
    if (allocate_one(&volume, &metadata_cursor, &data_dir_block) != 0 ||
        allocate_one(&volume, &metadata_cursor, &files_dir_block) != 0 ||
        allocate_one(&volume, &metadata_cursor, &fragmented_dir_block) != 0) goto cleanup_volume;
    init_directory_block(data_dir, data_dir_block, volume.root, "Defragmenter-TestData");
    init_directory_block(files_dir, files_dir_block, data_dir_block, "fragmented-files");
    init_directory_block(fragmented_dir, fragmented_dir_block, data_dir_block, "fragmented-directory");

    for (file_index = 0U; file_index < profile->files; ++file_index) {
        const uint32_t total_data_blocks = data_blocks[file_index];
        const uint32_t list_count = total_data_blocks > AMIGA_HASH_SIZE
            ? (total_data_blocks - AMIGA_HASH_SIZE + AMIGA_HASH_SIZE - 1U) / AMIGA_HASH_SIZE
            : 0U;
        (void)snprintf(targets[file_index].name, sizeof(targets[file_index].name),
                       "fragmented-%02zu.bin", file_index);
        if (allocate_one(&volume, &metadata_cursor, &targets[file_index].header) != 0) goto cleanup_volume;
        for (index = 0U; index < list_count; ++index) {
            uint32_t list_block;
            if (allocate_one(&volume, &metadata_cursor, &list_block) != 0 ||
                block_vec_push(&targets[file_index].lists, list_block) != 0) goto cleanup_volume;
        }
    }

    for (index = 0U; index < profile->directory_initial; ++index) {
        uint32_t header;
        if (allocate_one(&volume, &metadata_cursor, &header) != 0) goto cleanup_volume;
        if ((index & 1U) == 0U) {
            unsigned char zero[AMIGA_BLOCK_SIZE] = {0};
            ld_bitmap_set(volume.free_map, header, true);
            if (write_block(volume.fd, header, zero) != 0) goto cleanup_volume;
        } else {
            unsigned char header_block[AMIGA_BLOCK_SIZE];
            char name[32];
            (void)snprintf(name, sizeof(name), "entry-%05u.txt", index);
            init_empty_file_header(header_block, header, fragmented_dir_block, name);
            if (write_block(volume.fd, header, header_block) != 0 ||
                link_child(volume.fd, fragmented_dir, header, name) != 0) goto cleanup_volume;
        }
    }
    for (index = 0U; index < profile->directory_second; ++index) {
        uint32_t header;
        unsigned char header_block[AMIGA_BLOCK_SIZE];
        char name[32];
        const uint32_t entry_index = profile->directory_initial + index;
        if (allocate_one(&volume, &metadata_cursor, &header) != 0) goto cleanup_volume;
        (void)snprintf(name, sizeof(name), "entry-%05u.txt", entry_index);
        init_empty_file_header(header_block, header, fragmented_dir_block, name);
        if (write_block(volume.fd, header, header_block) != 0 ||
            link_child(volume.fd, fragmented_dir, header, name) != 0) goto cleanup_volume;
    }

    data_cursor = volume.blocks / 8U;
    if (data_cursor < metadata_cursor + 1024U) data_cursor = metadata_cursor + 1024U;
    for (index = 0U; index < profile->chunks; ++index) {
        for (file_index = 0U; file_index < profile->files; ++file_index) {
            const uint32_t chunks = ldtm_profile_file_chunks(profile, file_index);
            if (index >= chunks) continue;
            const uint32_t total_data_blocks = data_blocks[file_index];
            const uint32_t base = total_data_blocks / chunks;
            const uint32_t extra = total_data_blocks % chunks;
            const uint32_t run = base + (index < extra ? 1U : 0U);
            if (run == 0U ||
                allocate_run(&volume, &data_cursor, run,
                             &targets[file_index].data) != 0)
                goto cleanup_volume;
        }
    }

    for (file_index = 0U; file_index < profile->files; ++file_index) {
        if (targets[file_index].data.count != data_blocks[file_index] ||
            write_target_data(&volume, &targets[file_index], dostype,
                              (uint32_t)file_index, file_sizes[file_index]) != 0 ||
            write_target_metadata(&volume, &targets[file_index], files_dir_block,
                                  (uint32_t)file_sizes[file_index]) != 0 ||
            link_child(volume.fd, files_dir, targets[file_index].header, targets[file_index].name) != 0) {
            goto cleanup_volume;
        }
    }

    fix_checksum(files_dir, 5U);
    fix_checksum(fragmented_dir, 5U);
    if (write_block(volume.fd, files_dir_block, files_dir) != 0 ||
        write_block(volume.fd, fragmented_dir_block, fragmented_dir) != 0) goto cleanup_volume;
    if (link_child(volume.fd, data_dir, files_dir_block, "fragmented-files") != 0 ||
        link_child(volume.fd, data_dir, fragmented_dir_block, "fragmented-directory") != 0) goto cleanup_volume;
    fix_checksum(data_dir, 5U);
    if (write_block(volume.fd, data_dir_block, data_dir) != 0) goto cleanup_volume;
    if (read_block(volume.fd, volume.root, root) != 0 ||
        link_child(volume.fd, root, data_dir_block, "Defragmenter-TestData") != 0) goto cleanup_volume;
    fix_checksum(root, 5U);
    if (write_block(volume.fd, volume.root, root) != 0 || rewrite_bitmap(&volume) != 0 || fsync(volume.fd) != 0) {
        goto cleanup_volume;
    }
    result = 0;
cleanup_volume:
    affs_close(&volume);
cleanup_error:
    free(error);
    for (file_index = 0U; file_index < AMIGA_MAX_TARGET_FILES; ++file_index) {
        block_vec_free(&targets[file_index].data);
        block_vec_free(&targets[file_index].lists);
    }
    return result;
}

static int verify_target_data(const AffsVolume *volume, const AffsFile *file,
                              uint8_t dostype, uint32_t file_index, uint64_t file_size) {
    const size_t payload_size = dostype == 0U ? AMIGA_BLOCK_SIZE - 24U : AMIGA_BLOCK_SIZE;
    const size_t expected_blocks = (size_t)((file_size + payload_size - 1U) / payload_size);
    uint64_t remaining = file_size;
    size_t data_index;
    if ((uint64_t)file->byte_size != file_size || file->data.n != expected_blocks) return -1;
    for (data_index = 0U; data_index < file->data.n; ++data_index) {
        unsigned char block[AMIGA_BLOCK_SIZE];
        unsigned char expected[AMIGA_BLOCK_SIZE];
        const size_t take = remaining < payload_size ? (size_t)remaining : payload_size;
        if (read_block(volume->fd, file->data.v[data_index], block) != 0) return -1;
        deterministic_payload(expected, take, file_index, (uint32_t)data_index);
        if (dostype == 0U) {
            const uint32_t next = data_index + 1U < file->data.n ? file->data.v[data_index + 1U] : 0U;
            if (block_word(block, 0U) != AMIGA_TYPE_DATA || block_word(block, 1U) != file->header ||
                block_word(block, 2U) != (uint32_t)data_index + 1U ||
                block_word(block, 3U) != (uint32_t)take || block_word(block, 4U) != next ||
                !checksum_ok(block) || memcmp(block + 24U, expected, take) != 0) return -1;
        } else if (memcmp(block, expected, take) != 0) {
            return -1;
        }
        remaining -= take;
    }
    return remaining == 0U ? 0 : -1;
}

static int verify_amiga_payload_state(
    const char *path, uint8_t dostype,
    const LdtmFragmentProfile *profile, int expect_fragmented,
    char *detail, size_t detail_capacity) {
    AffsVolume volume;
    char *error = NULL;
    uint8_t seen[AMIGA_MAX_TARGET_FILES] = {0};
    uint32_t expected_directory_entries;
    uint32_t directory_entries = 0U;
    size_t file_index;
    int result = -1;
    if (detail != NULL && detail_capacity > 0U) detail[0] = '\0';
    if (path == NULL || profile == NULL || (dostype != 0U && dostype != 1U) ||
        profile->files == 0U || profile->files > AMIGA_MAX_TARGET_FILES) return -1;
    expected_directory_entries = profile->directory_initial / 2U + profile->directory_second;
    if (affs_scan(path, false, &volume, &error) != 0) goto cleanup_error;
    if (volume.dostype != dostype || volume.directory_blocks.n != 4U) goto cleanup_volume;
    for (file_index = 0U; file_index < volume.files.n; ++file_index) {
        const AffsFile *file = &volume.files.v[file_index];
        unsigned char header[AMIGA_BLOCK_SIZE];
        char name[32];
        unsigned target_index;
        int consumed = 0;
        if (read_block(volume.fd, file->header, header) != 0) goto cleanup_volume;
        get_bstr_name(header, name);
        if (sscanf(name, "fragmented-%02u.bin%n", &target_index, &consumed) == 1 &&
            consumed > 0 && name[consumed] == '\0' && target_index < profile->files) {
            const size_t fragments = affs_fragments(&file->data);
            const uint64_t file_size =
                ldtm_profile_file_bytes(profile, target_index);
            if (seen[target_index] != 0U ||
                (expect_fragmented != 0
                    ? fragments < 2U
                    : fragments != 1U) ||
                verify_target_data(&volume, file, dostype, target_index,
                                   file_size) != 0)
                goto cleanup_volume;
            seen[target_index] = 1U;
        } else if (infiltratr_string_starts_with(name, "entry-") &&
                   file->byte_size == 0U && file->data.n == 0U) {
            ++directory_entries;
        }
    }
    for (file_index = 0U; file_index < profile->files; ++file_index) {
        if (seen[file_index] == 0U) goto cleanup_volume;
    }
    if (directory_entries != expected_directory_entries ||
        volume.files.n != (size_t)profile->files + (size_t)expected_directory_entries)
        goto cleanup_volume;
    if (detail != NULL && detail_capacity > 0U) {
        if (expect_fragmented != 0) {
            (void)snprintf(
                detail, detail_capacity,
                "raw C Amiga heterogeneous payload verified: %u differently sized targets, %llu MiB total, every target fragmented, %u retained directory entries",
                profile->files,
                (unsigned long long)(ldtm_profile_payload_bytes(profile) / LDTM_MIB),
                expected_directory_entries);
        } else {
            (void)snprintf(
                detail, detail_capacity,
                "raw C Amiga post-defrag payload verified byte-for-byte: %u targets are contiguous and %u retained directory entries match",
                profile->files, expected_directory_entries);
        }
    }
    result = 0;
cleanup_volume:
    affs_close(&volume);
cleanup_error:
    if (result != 0 && detail != NULL && detail_capacity > 0U && detail[0] == '\0') {
        (void)snprintf(detail, detail_capacity, "%s",
                       error != NULL ? error : "raw Amiga payload validation failed");
    }
    free(error);
    return result;
}

int ldtm_verify_amiga_payload(const char *path, uint8_t dostype,
                              const LdtmFragmentProfile *profile,
                              char *detail, size_t detail_capacity) {
    return verify_amiga_payload_state(
        path, dostype, profile, 1, detail, detail_capacity);
}

int ldtm_verify_amiga_payload_affs_after_defrag(
    const char *path, uint8_t dostype,
    const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity) {
    return verify_amiga_payload_state(
        path, dostype, profile, 0, detail, detail_capacity);
}
