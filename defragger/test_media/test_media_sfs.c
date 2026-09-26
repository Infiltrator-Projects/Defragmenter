// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "sfs_native.h"

#include <infiltratr/endian.h>
#include <infiltratr/posix_io.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SFS_TM_BLOCK_SIZE 4096U
#define SFS_TM_HEADER_BYTES 12U
#define SFS_TM_CAP_BYTES (UINT64_C(2) * LDTM_GIB)
#define SFS_TM_BITMAP_BASE 1U
#define SFS_TM_FILE_ID 10U
#define SFS_TM_FRAGMENTS 100U
#define SFS_TM_CHUNK_BLOCKS 512U
#define SFS_TM_CHUNK_KIB 2048U
#define SFS_TM_LOW_START 128U
#define SFS_TM_LOW_STRIDE 768U
#define SFS_TM_DATA_BLOCKS (SFS_TM_FRAGMENTS * SFS_TM_CHUNK_BLOCKS)

typedef struct {
    uint32_t total_blocks;
    uint32_t bitmap_blocks;
    uint32_t admin;
    uint32_t extents;
    uint32_t objects;
    uint32_t object_nodes;
} SfsTmGeometry;

static uint32_t load_be32(const uint8_t *p)
{
    return infiltratr_load_be32(p);
}

static void stamp_checksum(uint8_t *block)
{
    uint32_t sum = 1U;
    infiltratr_store_be32(block + 4U, 0U);
    for (uint32_t offset = 0U; offset < SFS_TM_BLOCK_SIZE; offset += 4U)
        sum += infiltratr_load_be32(block + offset);
    infiltratr_store_be32(block + 4U, 0U - sum);
}

static void set_header(uint8_t *block, const char id[4], uint32_t own_block)
{
    memcpy(block, id, 4U);
    infiltratr_store_be32(block + 8U, own_block);
}

static int geometry_for_path(const char *path, SfsTmGeometry *geometry)
{
    if (path == NULL || geometry == NULL)
        return -1;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const off_t end = lseek(fd, 0, SEEK_END);
    (void)close(fd);
    if (end <= 0)
        return -1;
    uint64_t bytes = (uint64_t)end;
    if (bytes > SFS_TM_CAP_BYTES)
        bytes = SFS_TM_CAP_BYTES;
    if (bytes % SFS_TM_BLOCK_SIZE != 0U)
        return -1;
    const uint64_t blocks64 = bytes / SFS_TM_BLOCK_SIZE;
    if (blocks64 > UINT32_MAX || blocks64 < UINT64_C(16384))
        return -1;

    const uint64_t bits_per =
        (uint64_t)(SFS_TM_BLOCK_SIZE - SFS_TM_HEADER_BYTES) * 8U;
    const uint64_t bitmap_blocks =
        (blocks64 + bits_per - 1U) / bits_per;
    if (bitmap_blocks == 0U || bitmap_blocks > UINT32_MAX)
        return -1;

    SfsTmGeometry value = {0};
    value.total_blocks = (uint32_t)blocks64;
    value.bitmap_blocks = (uint32_t)bitmap_blocks;
    value.admin = SFS_TM_BITMAP_BASE + value.bitmap_blocks;
    value.extents = value.admin + 1U;
    value.objects = value.extents + 1U;
    value.object_nodes = value.objects + 1U;
    if (value.object_nodes + 2U >= value.total_blocks - 1U)
        return -1;
    *geometry = value;
    return 0;
}

static uint32_t fragment_start(const SfsTmGeometry *geometry, uint32_t index)
{
    if (index + 1U == SFS_TM_FRAGMENTS)
        return geometry->total_blocks - (SFS_TM_CHUNK_BLOCKS + 1024U);
    return SFS_TM_LOW_START + index * SFS_TM_LOW_STRIDE;
}

static int profile_matches(const LdtmFragmentProfile *profile)
{
    if (profile == NULL ||
        profile->files != LDTM_TARGET_FILE_COUNT ||
        profile->chunks != 30U ||
        profile->chunk_kib != SFS_TM_CHUNK_KIB ||
        profile->directory_initial != 0U ||
        profile->directory_second != 0U)
        return 0;
    static const uint32_t expected[LDTM_TARGET_FILE_COUNT] =
        {2U, 3U, 5U, 8U, 13U, 17U, 22U, 30U};
    for (size_t file = 0U; file < LDTM_TARGET_FILE_COUNT; ++file)
        if (ldtm_profile_file_chunks(profile, file) != expected[file])
            return 0;
    return ldtm_profile_payload_bytes(profile) == UINT64_C(200) * LDTM_MIB;
}

static int fragment_owner(const LdtmFragmentProfile *profile,
                          uint32_t fragment,
                          uint32_t *file_index,
                          uint32_t *within_file)
{
    uint32_t first = 0U;
    for (uint32_t file = 0U; file < profile->files; ++file) {
        const uint32_t count = ldtm_profile_file_chunks(profile, file);
        if (fragment < first + count) {
            if (file_index != NULL) *file_index = file;
            if (within_file != NULL) *within_file = fragment - first;
            return 0;
        }
        first += count;
    }
    return -1;
}

static uint32_t first_fragment_for_file(const LdtmFragmentProfile *profile,
                                        uint32_t file_index)
{
    uint32_t first = 0U;
    for (uint32_t file = 0U; file < file_index; ++file)
        first += ldtm_profile_file_chunks(profile, file);
    return first;
}

static int geometry_is_used(const SfsTmGeometry *geometry, uint32_t block)
{
    if (block == 0U || block == geometry->total_blocks - 1U)
        return 1;
    if (block >= SFS_TM_BITMAP_BASE &&
        block < SFS_TM_BITMAP_BASE + geometry->bitmap_blocks)
        return 1;
    if (block == geometry->admin || block == geometry->extents ||
        block == geometry->objects || block == geometry->object_nodes)
        return 1;
    for (uint32_t fragment = 0U; fragment < SFS_TM_FRAGMENTS; ++fragment) {
        const uint32_t start = fragment_start(geometry, fragment);
        if (block >= start && block < start + SFS_TM_CHUNK_BLOCKS)
            return 1;
    }
    return 0;
}

static void make_root(uint8_t *block, uint32_t own_block, uint16_t sequence,
                      const SfsTmGeometry *geometry)
{
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "SFS\0", own_block);
    infiltratr_store_be16(block + 12U, 3U);
    infiltratr_store_be16(block + 14U, sequence);
    infiltratr_store_be32(block + 48U, geometry->total_blocks);
    infiltratr_store_be32(block + 52U, SFS_TM_BLOCK_SIZE);
    infiltratr_store_be32(block + 96U, SFS_TM_BITMAP_BASE);
    infiltratr_store_be32(block + 100U, geometry->admin);
    infiltratr_store_be32(block + 104U, geometry->objects);
    infiltratr_store_be32(block + 108U, geometry->extents);
    infiltratr_store_be32(block + 112U, geometry->object_nodes);
    stamp_checksum(block);
}

static void bitmap_set_free(uint8_t *block, uint32_t bit, int is_free)
{
    const uint8_t mask = (uint8_t)(0x80U >> (bit & 7U));
    uint8_t *slot = block + SFS_TM_HEADER_BYTES + bit / 8U;
    if (is_free != 0)
        *slot |= mask;
    else
        *slot &= (uint8_t)~mask;
}

static void make_bitmap(uint8_t *block, const SfsTmGeometry *geometry,
                        uint32_t sequence)
{
    const uint32_t own = SFS_TM_BITMAP_BASE + sequence;
    const uint64_t bits_per =
        (uint64_t)(SFS_TM_BLOCK_SIZE - SFS_TM_HEADER_BYTES) * 8U;
    const uint64_t first = (uint64_t)sequence * bits_per;
    uint64_t count = bits_per;
    if (first + count > geometry->total_blocks)
        count = geometry->total_blocks - first;

    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "BTMP", own);
    for (uint64_t bit = 0U; bit < count; ++bit) {
        const uint32_t fs_block = (uint32_t)(first + bit);
        bitmap_set_free(block, (uint32_t)bit,
                        geometry_is_used(geometry, fs_block) ? 0 : 1);
    }
    stamp_checksum(block);
}

static void make_extent_tree(uint8_t *block, const SfsTmGeometry *geometry,
                             const LdtmFragmentProfile *profile)
{
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "BNDC", geometry->extents);
    infiltratr_store_be16(block + 12U, SFS_TM_FRAGMENTS);
    block[14U] = 1U;
    block[15U] = 14U;
    for (uint32_t index = 0U; index < SFS_TM_FRAGMENTS; ++index) {
        uint32_t file = 0U;
        uint32_t within = 0U;
        if (fragment_owner(profile, index, &file, &within) != 0)
            continue;
        const uint32_t chunks = ldtm_profile_file_chunks(profile, file);
        uint8_t *node = block + 16U + (size_t)index * 14U;
        const uint32_t start = fragment_start(geometry, index);
        infiltratr_store_be32(node, start);
        infiltratr_store_be32(
            node + 4U,
            within + 1U < chunks
                ? fragment_start(geometry, index + 1U) : 0U);
        infiltratr_store_be32(
            node + 8U,
            within > 0U ? fragment_start(geometry, index - 1U) : 0U);
        infiltratr_store_be16(node + 12U, SFS_TM_CHUNK_BLOCKS);
    }
    stamp_checksum(block);
}

static void make_object_container(uint8_t *block,
                                  const SfsTmGeometry *geometry,
                                  const LdtmFragmentProfile *profile)
{
    memset(block, 0, SFS_TM_BLOCK_SIZE);
    set_header(block, "OBJC", geometry->objects);
    size_t offset = 24U;
    for (uint32_t file = 0U; file < profile->files; ++file) {
        char name[32];
        const uint64_t file_bytes = ldtm_profile_file_bytes(profile, file);
        const uint32_t first = first_fragment_for_file(profile, file);
        if (file_bytes == 0U || file_bytes > UINT32_MAX ||
            offset + 44U > SFS_TM_BLOCK_SIZE)
            return;
        uint8_t *object = block + offset;
        infiltratr_store_be32(object + 4U, SFS_TM_FILE_ID + file);
        infiltratr_store_be32(object + 8U, 0x0fU);
        infiltratr_store_be32(object + 12U, fragment_start(geometry, first));
        infiltratr_store_be32(object + 16U, (uint32_t)file_bytes);
        object[24U] = 0U;
        (void)snprintf(name, sizeof(name), "fragmented-%02u.bin", file);
        const size_t name_length = strlen(name);
        memcpy(object + 25U, name, name_length);
        object[25U + name_length] = 0U;
        object[26U + name_length] = 0U;
        offset += 27U + name_length;
        if ((offset & 1U) != 0U) ++offset;
    }
    stamp_checksum(block);
}

static void make_payload(uint8_t *block, uint32_t file_index,
                         uint32_t logical_block)
{
    uint64_t state = UINT64_C(0x6a09e667f3bcc909) ^
                     ((uint64_t)file_index << 40U) ^
                     ((uint64_t)logical_block *
                      UINT64_C(0x9e3779b97f4a7c15));
    for (size_t offset = 0U; offset < SFS_TM_BLOCK_SIZE; ++offset) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        state *= UINT64_C(0x2545f4914f6cdd1d);
        block[offset] = (uint8_t)(state >> 56U);
    }
}

static int write_block(int fd, uint32_t block_number, const uint8_t *block)
{
    return infiltratr_pwrite_full(
        fd, block, SFS_TM_BLOCK_SIZE,
        (uint64_t)block_number * SFS_TM_BLOCK_SIZE);
}

static int read_block(int fd, uint32_t block_number, uint8_t *block)
{
    return infiltratr_pread_full(
        fd, block, SFS_TM_BLOCK_SIZE,
        (uint64_t)block_number * SFS_TM_BLOCK_SIZE);
}

int ldtm_format_sfs_volume(const char *path)
{
    const LdtmFilesystemSpec *spec = ldtm_find_spec("sfs");
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    SfsTmGeometry geometry;
    if (spec == NULL || !profile_matches(&profile) ||
        geometry_for_path(path, &geometry) != 0)
        return -1;

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    uint8_t *block = malloc(SFS_TM_BLOCK_SIZE);
    if (block == NULL) {
        (void)close(fd);
        return -1;
    }
    int result = -1;

    make_root(block, 0U, 5U, &geometry);
    if (write_block(fd, 0U, block) != 0)
        goto cleanup;

    for (uint32_t sequence = 0U; sequence < geometry.bitmap_blocks; ++sequence) {
        make_bitmap(block, &geometry, sequence);
        if (write_block(fd, SFS_TM_BITMAP_BASE + sequence, block) != 0)
            goto cleanup;
    }

    memset(block, 0, SFS_TM_BLOCK_SIZE);
    if (write_block(fd, geometry.admin, block) != 0 ||
        write_block(fd, geometry.object_nodes, block) != 0 ||
        write_block(fd, geometry.objects + 2U, block) != 0)
        goto cleanup;

    make_extent_tree(block, &geometry, &profile);
    if (write_block(fd, geometry.extents, block) != 0)
        goto cleanup;
    make_object_container(block, &geometry, &profile);
    if (write_block(fd, geometry.objects, block) != 0)
        goto cleanup;

    for (uint32_t fragment = 0U; fragment < SFS_TM_FRAGMENTS; ++fragment) {
        uint32_t file = 0U;
        uint32_t within_file = 0U;
        if (fragment_owner(&profile, fragment, &file, &within_file) != 0)
            goto cleanup;
        const uint32_t start = fragment_start(&geometry, fragment);
        for (uint32_t within = 0U; within < SFS_TM_CHUNK_BLOCKS; ++within) {
            make_payload(
                block, file,
                within_file * SFS_TM_CHUNK_BLOCKS + within);
            if (write_block(fd, start + within, block) != 0)
                goto cleanup;
        }
    }

    make_root(block, geometry.total_blocks - 1U, 6U, &geometry);
    if (write_block(fd, geometry.total_blocks - 1U, block) != 0 ||
        fsync(fd) != 0)
        goto cleanup;
    result = 0;

cleanup:
    free(block);
    (void)close(fd);
    return result;
}

int ldtm_populate_sfs_volume(const char *path,
                             const LdtmFragmentProfile *profile)
{
    SfsAnalysis analysis;
    char error[256] = {0};
    if (!profile_matches(profile))
        return -1;
    return sfs_analyse(path, &analysis, NULL, 0U,
                       error, sizeof(error)) == 0 &&
           analysis.regular_files == LDTM_TARGET_FILE_COUNT &&
           analysis.data_blocks == SFS_TM_DATA_BLOCKS &&
           analysis.fragmented_files == LDTM_TARGET_FILE_COUNT ? 0 : -1;
}

static int verify_current_payload(const char *path,
                                  const LdtmFragmentProfile *profile,
                                  int expect_fragmented)
{
    uint8_t *root = NULL;
    uint8_t *objects = NULL;
    uint8_t *extent_tree = NULL;
    uint8_t *actual = NULL;
    uint8_t *expected = NULL;
    int fd = -1;
    int result = -1;

    root = malloc(SFS_TM_BLOCK_SIZE);
    objects = malloc(SFS_TM_BLOCK_SIZE);
    extent_tree = malloc(SFS_TM_BLOCK_SIZE);
    actual = malloc(SFS_TM_BLOCK_SIZE);
    expected = malloc(SFS_TM_BLOCK_SIZE);
    if (root == NULL || objects == NULL || extent_tree == NULL ||
        actual == NULL || expected == NULL)
        goto cleanup;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read_block(fd, 0U, root) != 0)
        goto cleanup;
    if (memcmp(root, "SFS\0", 4U) != 0 ||
        load_be32(root + 52U) != SFS_TM_BLOCK_SIZE)
        goto cleanup;

    const uint32_t total_blocks = load_be32(root + 48U);
    const uint32_t object_block = load_be32(root + 104U);
    const uint32_t extent_block = load_be32(root + 108U);
    if (total_blocks < 16384U || object_block >= total_blocks ||
        extent_block >= total_blocks ||
        read_block(fd, object_block, objects) != 0 ||
        read_block(fd, extent_block, extent_tree) != 0)
        goto cleanup;
    if (memcmp(objects, "OBJC", 4U) != 0 ||
        memcmp(extent_tree, "BNDC", 4U) != 0 ||
        extent_tree[14U] == 0U || extent_tree[15U] != 14U)
        goto cleanup;

    const uint16_t extent_count = infiltratr_load_be16(extent_tree + 12U);
    size_t object_offset = 24U;
    for (uint32_t file = 0U; file < profile->files; ++file) {
        if (object_offset + 27U > SFS_TM_BLOCK_SIZE)
            goto cleanup;
        const uint8_t *object = objects + object_offset;
        const uint64_t expected_bytes =
            ldtm_profile_file_bytes(profile, file);
        if (expected_bytes == 0U || expected_bytes > UINT32_MAX ||
            load_be32(object + 4U) != SFS_TM_FILE_ID + file ||
            load_be32(object + 16U) != (uint32_t)expected_bytes)
            goto cleanup;

        uint32_t key = load_be32(object + 12U);
        uint32_t previous = 0U;
        uint32_t previous_end = 0U;
        uint64_t logical_blocks = 0U;
        uint32_t chain_count = 0U;
        int physically_fragmented = 0;
        while (key != 0U) {
            if (++chain_count > SFS_TM_FRAGMENTS)
                goto cleanup;
            const uint8_t *record = NULL;
            for (uint16_t index = 0U; index < extent_count; ++index) {
                const uint8_t *candidate =
                    extent_tree + 16U + (size_t)index * 14U;
                if (load_be32(candidate) == key) {
                    record = candidate;
                    break;
                }
            }
            if (record == NULL || load_be32(record + 8U) != previous)
                goto cleanup;
            const uint32_t blocks = infiltratr_load_be16(record + 12U);
            const uint32_t next = load_be32(record + 4U);
            if (blocks == 0U || key >= total_blocks ||
                blocks > total_blocks - key)
                goto cleanup;
            if (chain_count > 1U && key != previous_end)
                physically_fragmented = 1;
            previous_end = key + blocks;

            for (uint32_t within = 0U; within < blocks; ++within) {
                if ((logical_blocks + 1U) * SFS_TM_BLOCK_SIZE >
                        expected_bytes ||
                    read_block(fd, key + within, actual) != 0)
                    goto cleanup;
                make_payload(expected, file, (uint32_t)logical_blocks);
                if (memcmp(actual, expected, SFS_TM_BLOCK_SIZE) != 0)
                    goto cleanup;
                ++logical_blocks;
            }
            previous = key;
            key = next;
        }
        if (logical_blocks * SFS_TM_BLOCK_SIZE != expected_bytes ||
            (expect_fragmented != 0
                ? !physically_fragmented
                : physically_fragmented))
            goto cleanup;

        const size_t name_offset = object_offset + 25U;
        const uint8_t *limit = objects + SFS_TM_BLOCK_SIZE;
        const uint8_t *name_end =
            memchr(objects + name_offset, 0,
                   (size_t)(limit - (objects + name_offset)));
        if (name_end == NULL)
            goto cleanup;
        const uint8_t *comment = name_end + 1U;
        const uint8_t *comment_end =
            comment < limit
                ? memchr(comment, 0, (size_t)(limit - comment))
                : NULL;
        if (comment_end == NULL)
            goto cleanup;
        object_offset = (size_t)(comment_end - objects) + 1U;
        if ((object_offset & 1U) != 0U) ++object_offset;
    }
    result = 0;

cleanup:
    if (fd >= 0)
        (void)close(fd);
    free(expected);
    free(actual);
    free(extent_tree);
    free(objects);
    free(root);
    return result;
}

static int verify_sfs_payload_state(const char *path,
                                    const LdtmFragmentProfile *profile,
                                    int expect_fragmented,
                                    char *detail, size_t detail_capacity)
{
    SfsAnalysis analysis;
    SfsTmGeometry geometry;
    char error[256] = {0};
    int result = -1;
    if (detail != NULL && detail_capacity > 0U)
        detail[0] = '\0';
    if (!profile_matches(profile) ||
        geometry_for_path(path, &geometry) != 0 ||
        !sfs_probe(path) ||
        sfs_analyse(path, &analysis, NULL, 0U,
                    error, sizeof(error)) != 0)
        goto cleanup;

    if (analysis.block_size != SFS_TM_BLOCK_SIZE ||
        analysis.total_blocks != geometry.total_blocks ||
        analysis.data_blocks != SFS_TM_DATA_BLOCKS ||
        analysis.regular_files != LDTM_TARGET_FILE_COUNT ||
        analysis.fragmented_files !=
            (expect_fragmented != 0 ? LDTM_TARGET_FILE_COUNT : 0U) ||
        !analysis.primary_root_valid || !analysis.backup_root_valid ||
        analysis.transaction_pending ||
        verify_current_payload(path, profile, expect_fragmented) != 0)
        goto cleanup;

    if (detail != NULL && detail_capacity > 0U) {
        (void)snprintf(
            detail, detail_capacity,
            expect_fragmented != 0
                ? "native SFS0 payload verified on %u MiB media: 8 differently sized fragmented files, 200 MiB total"
                : "native SFS0 post-defrag payload verified byte-for-byte; production analyser reports zero fragmented files",
            (unsigned)((uint64_t)geometry.total_blocks *
                       SFS_TM_BLOCK_SIZE / LDTM_MIB));
    }
    result = 0;

cleanup:
    if (result != 0 && detail != NULL && detail_capacity > 0U &&
        detail[0] == '\0')
        (void)snprintf(detail, detail_capacity, "%s",
                       error[0] != '\0'
                           ? error : "native SFS0 payload validation failed");
    return result;
}

int ldtm_verify_sfs_payload(const char *path,
                            const LdtmFragmentProfile *profile,
                            char *detail, size_t detail_capacity)
{
    return verify_sfs_payload_state(path, profile, 1,
                                    detail, detail_capacity);
}

int ldtm_verify_sfs_payload_after_defrag(
    const char *path, const LdtmFragmentProfile *profile,
    char *detail, size_t detail_capacity)
{
    return verify_sfs_payload_state(path, profile, 0,
                                    detail, detail_capacity);
}
