// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fat_io.h"
#include "fat_volume.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(                                                         \
                stderr,                                                      \
                "%s:%d: check failed: %s\n",                                 \
                __FILE__,                                                    \
                __LINE__,                                                    \
                #condition                                                   \
            );                                                               \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

static void put_u16(uint8_t *buffer, size_t offset, uint16_t value) {
    buffer[offset] = (uint8_t)(value & UINT16_C(0x00FF));
    buffer[offset + 1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *buffer, size_t offset, uint32_t value) {
    put_u16(buffer, offset, (uint16_t)(value & UINT32_C(0x0000FFFF)));
    put_u16(buffer, offset + 2, (uint16_t)(value >> 16));
}

static void make_boot_sector(uint8_t boot[512], FatType type) {
    memset(boot, 0, 512);
    put_u16(boot, 11, 512);
    boot[16] = 2;
    boot[510] = 0x55;
    boot[511] = 0xAA;
    if (type == FAT_TYPE_12) {
        boot[13] = 1;
        put_u16(boot, 14, 1);
        put_u16(boot, 17, 224);
        put_u16(boot, 19, 2880);
        put_u16(boot, 22, 9);
        put_u32(boot, 39, UINT32_C(0x12345678));
    } else if (type == FAT_TYPE_16) {
        boot[13] = 4;
        put_u16(boot, 14, 1);
        put_u16(boot, 17, 512);
        put_u32(boot, 32, 65536);
        put_u16(boot, 22, 64);
        put_u32(boot, 39, UINT32_C(0x87654321));
    } else {
        boot[13] = 8;
        put_u16(boot, 14, 32);
        put_u32(boot, 32, 1048576);
        put_u32(boot, 36, 1024);
        put_u16(boot, 40, 0);
        put_u16(boot, 42, 0);
        put_u32(boot, 44, 2);
        put_u16(boot, 48, 1);
        put_u16(boot, 50, 6);
        put_u32(boot, 67, UINT32_C(0xA5A55A5A));
    }
}

static void check_geometry_invariants(const FatGeometry *geometry) {
    CHECK(geometry->fat_type == FAT_TYPE_12
          || geometry->fat_type == FAT_TYPE_16
          || geometry->fat_type == FAT_TYPE_32);
    CHECK(geometry->bytes_per_sector >= 512);
    CHECK((geometry->bytes_per_sector & (geometry->bytes_per_sector - 1)) == 0);
    CHECK(geometry->sectors_per_cluster != 0);
    CHECK((geometry->sectors_per_cluster & (geometry->sectors_per_cluster - 1)) == 0);
    CHECK(geometry->cluster_count != 0);
    CHECK(geometry->max_cluster == geometry->cluster_count + 1);
    CHECK(geometry->fat_entry_count > geometry->max_cluster);
    CHECK(geometry->volume_bytes != 0);
}

static uint32_t fuzz_next(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void test_authoritative_geometry_parser(void) {
    for (unsigned bits = 12; bits <= 32; bits += bits == 12 ? 4u : 16u) {
        uint8_t boot[512];
        FatType expected = bits == 12 ? FAT_TYPE_12 :
                           bits == 16 ? FAT_TYPE_16 : FAT_TYPE_32;
        make_boot_sector(boot, expected);
        FatGeometry geometry;
        char error[160];
        CHECK(fat_geometry_parse(boot, UINT64_MAX, &geometry, error, sizeof(error)));
        CHECK(geometry.fat_type == expected);
        check_geometry_invariants(&geometry);

        /* Each byte is perturbed once. Accepted variants must still satisfy
           every postcondition; rejected variants must fail without crashing. */
        uint32_t state = UINT32_C(0x4C444631) ^ bits;
        for (size_t offset = 0; offset < sizeof(boot); offset++) {
            uint8_t changed[512];
            memcpy(changed, boot, sizeof(changed));
            changed[offset] ^= (uint8_t)(1u << (fuzz_next(&state) & 7u));
            if (fat_geometry_parse(
                    changed, UINT64_MAX, &geometry, error, sizeof(error))) {
                check_geometry_invariants(&geometry);
            }
        }
    }

    uint32_t state = UINT32_C(0x9E3779B9);
    for (size_t sample = 0; sample < 20000; sample++) {
        uint8_t boot[512];
        for (size_t index = 0; index < sizeof(boot); index++) {
            boot[index] = (uint8_t)fuzz_next(&state);
        }
        FatGeometry geometry;
        char error[160];
        if (fat_geometry_parse(
                boot, UINT64_MAX, &geometry, error, sizeof(error))) {
            check_geometry_invariants(&geometry);
        }
    }
}

static void test_geometry_helpers(void) {
    Fat32 fs = {
        .fat_type = FAT_TYPE_12,
        .data_offset = UINT64_C(4096),
        .cluster_size = UINT64_C(2048),
        .max_cluster = 100,
    };

    CHECK(fat_mask(&fs) == UINT32_C(0x0FFF));
    CHECK(fat_eoc_min(&fs) == UINT32_C(0x0FF8));
    CHECK(fat_bad_value(&fs) == UINT32_C(0x0FF7));
    CHECK(strcmp(fat_type_name(&fs), "FAT12") == 0);
    CHECK(cluster_offset(&fs, 2) == UINT64_C(4096));
    CHECK(cluster_offset(&fs, 5) == UINT64_C(10240));

    fs.fat_type = FAT_TYPE_16;
    CHECK(fat_mask(&fs) == UINT32_C(0xFFFF));
    CHECK(fat_eoc_min(&fs) == UINT32_C(0xFFF8));
    CHECK(fat_bad_value(&fs) == UINT32_C(0xFFF7));
    CHECK(strcmp(fat_type_name(&fs), "FAT16") == 0);

    fs.fat_type = FAT_TYPE_32;
    CHECK(fat_mask(&fs) == FAT32_MASK);
    CHECK(fat_eoc_min(&fs) == FAT32_EOC_MIN);
    CHECK(fat_bad_value(&fs) == FAT32_BAD);
    CHECK(strcmp(fat_type_name(&fs), "FAT32") == 0);
}

static void test_vector_and_chain_traversal(void) {
    Fat32 fs = {
        .fat_type = FAT_TYPE_32,
        .cluster_count = 8,
        .max_cluster = 9,
        .fat_entry_count = 10,
    };
    fs.fat = calloc(fs.fat_entry_count, sizeof(*fs.fat));
    fs.chain_seen = calloc(
        (size_t)fs.max_cluster + 1,
        sizeof(*fs.chain_seen)
    );
    CHECK(fs.fat != NULL);
    CHECK(fs.chain_seen != NULL);

    fs.fat[2] = 4;
    fs.fat[4] = 5;
    fs.fat[5] = FAT32_MASK;
    U32Vec chain = fat32_read_chain(&fs, 2);
    CHECK(chain.len == 3);
    CHECK(chain.v[0] == 2);
    CHECK(chain.v[1] == 4);
    CHECK(chain.v[2] == 5);
    CHECK(fat_is_eoc_for(&fs, fs.fat[5]));
    CHECK(fat_is_free(&fs, 3));
    u32vec_free(&chain);
    CHECK(chain.v == NULL && chain.len == 0 && chain.cap == 0);

    free(fs.chain_seen);
    free(fs.fat);
}

static void test_buffered_cluster_io(void) {
    char path[] = "/tmp/defragmenter-fat-io.XXXXXX";
    int file_descriptor = mkstemp(path);
    CHECK(file_descriptor >= 0);
    CHECK(unlink(path) == 0);
    CHECK(ftruncate(file_descriptor, 8192) == 0);

    Fat32 fs = {
        .dev = {
            .fd = file_descriptor,
            .writable = true,
            .size_bytes = 8192,
        },
        .data_offset = 4096,
        .cluster_size = 512,
        .max_cluster = 9,
    };
    uint8_t source[1024];
    uint8_t destination[1024];
    for (size_t index = 0; index < sizeof(source); index++) {
        source[index] = (uint8_t)(index & 0xFFu);
    }
    CHECK(pwrite(
        file_descriptor,
        source,
        sizeof(source),
        (off_t)cluster_offset(&fs, 2)
    ) == (ssize_t)sizeof(source));

    const uint32_t sources[] = {2, 3};
    const uint32_t destinations[] = {5, 6};
    FatIoConfig config = {
        .ram_limit = 512,
        .workers = 2,
    };
    fat_io_copy_clusters(
        &fs,
        sources,
        destinations,
        2,
        &config
    );
    CHECK(pread(
        file_descriptor,
        destination,
        sizeof(destination),
        (off_t)cluster_offset(&fs, 5)
    ) == (ssize_t)sizeof(destination));
    CHECK(memcmp(source, destination, sizeof(source)) == 0);
    CHECK(config.bytes_read == sizeof(source));
    CHECK(config.bytes_written == sizeof(source));
    CHECK(config.read_extents == 2);
    CHECK(config.write_extents == 2);
    close(file_descriptor);
}

int main(void) {
    test_authoritative_geometry_parser();
    test_geometry_helpers();
    test_vector_and_chain_traversal();
    test_buffered_cluster_io();
    return 0;
}
