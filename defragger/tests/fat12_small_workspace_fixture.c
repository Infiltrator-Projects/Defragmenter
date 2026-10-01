// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native FAT12 regression fixture for very full relayouts.
 *
 * Three files occupy 1000 of 1200 data clusters.  The two 400-cluster files
 * are interlocked so neither final target is initially free, while only 200
 * clusters remain available for staging.  A correct writer must therefore
 * break the dependency at cluster granularity instead of requiring the largest
 * file to fit in the workspace.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BPS 512U
#define RESERVED 1U
#define FAT_COUNT 2U
#define ROOT_ENTRIES 32U
#define SECTORS_PER_FAT 4U
#define ROOT_SECTORS 2U
#define DATA_CLUSTERS 1200U
#define DATA_START_SECTOR (RESERVED + FAT_COUNT * SECTORS_PER_FAT + ROOT_SECTORS)
#define TOTAL_SECTORS (DATA_START_SECTOR + DATA_CLUSTERS)
#define MAX_CLUSTER (DATA_CLUSTERS + 1U)

#define A_CLUSTERS 400U
#define B_CLUSTERS 400U
#define C_CLUSTERS 200U

static void fail(const char *message) {
    if (errno != 0)
        perror(message);
    else
        fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xffU);
    p[1] = (uint8_t)(value >> 8U);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)(value & 0xffffU));
    put16(p + 2, (uint16_t)(value >> 16U));
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16U);
}

static void fat12_set(uint8_t *fat, uint16_t cluster, uint16_t value) {
    size_t offset = (size_t)cluster + (size_t)cluster / 2U;
    value &= 0x0fffU;
    if ((cluster & 1U) == 0U) {
        fat[offset] = (uint8_t)(value & 0xffU);
        fat[offset + 1] =
            (uint8_t)((fat[offset + 1] & 0xf0U) | ((value >> 8U) & 0x0fU));
    } else {
        uint8_t high_nibble =
            (uint8_t)((value & UINT16_C(0x000f)) << 4U);
        fat[offset] =
            (uint8_t)((uint8_t)(fat[offset] & UINT8_C(0x0f)) | high_nibble);
        fat[offset + 1] = (uint8_t)(value >> 4U);
    }
}

static uint16_t fat12_get(const uint8_t *fat, uint16_t cluster) {
    size_t offset = (size_t)cluster + (size_t)cluster / 2U;
    uint16_t value;
    if ((cluster & 1U) == 0U) {
        value = (uint16_t)fat[offset] |
                ((uint16_t)(fat[offset + 1] & 0x0fU) << 8U);
    } else {
        value = ((uint16_t)fat[offset] >> 4U) |
                ((uint16_t)fat[offset + 1] << 4U);
    }
    return value & 0x0fffU;
}

static size_t cluster_offset(uint16_t cluster) {
    return ((size_t)DATA_START_SECTOR + (size_t)(cluster - 2U)) * BPS;
}

static void fill_payload(uint8_t block[BPS], uint8_t file_id, uint32_t logical) {
    for (size_t index = 0; index < BPS; index++) {
        uint32_t value =
            (uint32_t)file_id * 73U + logical * 29U + (uint32_t)index * 17U;
        block[index] = (uint8_t)value;
    }
}

static void write_root_entry(uint8_t *entry, const char name[11],
                             uint16_t first_cluster, uint32_t bytes) {
    memset(entry, 0, 32U);
    memcpy(entry, name, 11U);
    entry[11] = 0x20U;
    put16(entry + 26U, first_cluster);
    put32(entry + 28U, bytes);
}

static void link_chain(uint8_t *fat, const uint16_t *chain, size_t count) {
    for (size_t index = 0; index < count; index++) {
        uint16_t next =
            index + 1U < count ? chain[index + 1U] : UINT16_C(0x0fff);
        fat12_set(fat, chain[index], next);
    }
}

static void write_chain_payload(uint8_t *image, const uint16_t *chain,
                                size_t count, uint8_t file_id) {
    uint8_t block[BPS];
    for (size_t logical = 0; logical < count; logical++) {
        fill_payload(block, file_id, (uint32_t)logical);
        memcpy(image + cluster_offset(chain[logical]), block, sizeof(block));
    }
}

static int create_image(const char *path) {
    const size_t image_bytes = (size_t)TOTAL_SECTORS * BPS;
    uint8_t *image = calloc(1U, image_bytes);
    if (image == NULL) fail("calloc image");

    uint8_t *boot = image;
    boot[0] = 0xebU;
    boot[1] = 0x3cU;
    boot[2] = 0x90U;
    memcpy(boot + 3U, "MSDOS5.0", 8U);
    put16(boot + 11U, BPS);
    boot[13] = 1U;
    put16(boot + 14U, RESERVED);
    boot[16] = FAT_COUNT;
    put16(boot + 17U, ROOT_ENTRIES);
    put16(boot + 19U, TOTAL_SECTORS);
    boot[21] = 0xf8U;
    put16(boot + 22U, SECTORS_PER_FAT);
    put16(boot + 24U, 32U);
    put16(boot + 26U, 64U);
    boot[36] = 0x80U;
    boot[38] = 0x29U;
    put32(boot + 39U, UINT32_C(0x12f00d12));
    memcpy(boot + 43U, "ROLLFAT12  ", 11U);
    memcpy(boot + 54U, "FAT12   ", 8U);
    boot[510] = 0x55U;
    boot[511] = 0xaaU;

    uint8_t fat[SECTORS_PER_FAT * BPS];
    memset(fat, 0, sizeof(fat));
    fat12_set(fat, 0U, UINT16_C(0x0ff8));
    fat12_set(fat, 1U, UINT16_C(0x0fff));

    uint16_t a[A_CLUSTERS];
    uint16_t b[B_CLUSTERS];
    uint16_t c[C_CLUSTERS];

    a[0] = 2U;
    for (size_t index = 1U; index < A_CLUSTERS; index++)
        a[index] = (uint16_t)(401U + index);

    for (size_t index = 0U; index + 1U < B_CLUSTERS; index++)
        b[index] = (uint16_t)(3U + index);
    b[B_CLUSTERS - 1U] = 801U;

    for (size_t index = 0U; index < C_CLUSTERS; index++)
        c[index] = (uint16_t)(802U + index);

    link_chain(fat, a, A_CLUSTERS);
    link_chain(fat, b, B_CLUSTERS);
    link_chain(fat, c, C_CLUSTERS);

    for (size_t copy = 0U; copy < FAT_COUNT; copy++) {
        size_t offset = (size_t)(RESERVED + copy * SECTORS_PER_FAT) * BPS;
        memcpy(image + offset, fat, sizeof(fat));
    }

    size_t root_offset =
        (size_t)(RESERVED + FAT_COUNT * SECTORS_PER_FAT) * BPS;
    write_root_entry(image + root_offset,
                     "AFILE   BIN", a[0], A_CLUSTERS * BPS);
    write_root_entry(image + root_offset + 32U,
                     "BFILE   BIN", b[0], B_CLUSTERS * BPS);
    write_root_entry(image + root_offset + 64U,
                     "CFILE   BIN", c[0], C_CLUSTERS * BPS);

    write_chain_payload(image, a, A_CLUSTERS, 1U);
    write_chain_payload(image, b, B_CLUSTERS, 2U);
    write_chain_payload(image, c, C_CLUSTERS, 3U);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) fail("open output");
    size_t written = 0U;
    while (written < image_bytes) {
        ssize_t count = write(fd, image + written, image_bytes - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            close(fd);
            free(image);
            fail("write output");
        }
        if (count == 0) {
            close(fd);
            free(image);
            errno = EIO;
            fail("short write");
        }
        written += (size_t)count;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        free(image);
        fail("sync output");
    }
    free(image);
    return EXIT_SUCCESS;
}

static uint8_t *read_image(const char *path, size_t *size_out) {
    struct stat st;
    if (stat(path, &st) != 0) fail("stat image");
    if (st.st_size != (off_t)((size_t)TOTAL_SECTORS * BPS)) {
        errno = 0;
        fail("unexpected FAT12 fixture size");
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail("open image");
    uint8_t *image = malloc((size_t)st.st_size);
    if (image == NULL) {
        close(fd);
        fail("malloc image");
    }
    size_t read_bytes = 0U;
    while (read_bytes < (size_t)st.st_size) {
        ssize_t count = read(fd, image + read_bytes,
                             (size_t)st.st_size - read_bytes);
        if (count < 0) {
            if (errno == EINTR) continue;
            close(fd);
            free(image);
            fail("read image");
        }
        if (count == 0) {
            close(fd);
            free(image);
            errno = EIO;
            fail("short read");
        }
        read_bytes += (size_t)count;
    }
    close(fd);
    *size_out = read_bytes;
    return image;
}

static void verify_file(const uint8_t *image, const uint8_t *fat,
                        const uint8_t *entry, uint16_t expected_start,
                        size_t expected_clusters, uint8_t file_id) {
    uint16_t cluster = get16(entry + 26U);
    uint32_t bytes = get32(entry + 28U);
    if (cluster != expected_start || bytes != expected_clusters * BPS) {
        errno = 0;
        fail("unexpected directory entry after relayout");
    }

    uint8_t expected[BPS];
    for (size_t logical = 0U; logical < expected_clusters; logical++) {
        uint16_t wanted = (uint16_t)(expected_start + logical);
        if (cluster != wanted) {
            errno = 0;
            fail("file chain is not in canonical contiguous order");
        }
        fill_payload(expected, file_id, (uint32_t)logical);
        if (memcmp(image + cluster_offset(cluster), expected, BPS) != 0) {
            errno = 0;
            fail("payload changed during FAT12 relayout");
        }
        uint16_t next = fat12_get(fat, cluster);
        if (logical + 1U == expected_clusters) {
            if (next < 0x0ff8U) {
                errno = 0;
                fail("file chain does not terminate with FAT12 EOC");
            }
        } else if (next != (uint16_t)(cluster + 1U)) {
            errno = 0;
            fail("file chain link is not contiguous");
        }
        cluster = next;
    }
}

static void verify_free_run(const uint8_t *fat, uint16_t first, uint16_t last) {
    for (uint16_t cluster = first; cluster <= last; cluster++) {
        if (fat12_get(fat, cluster) != 0U) {
            errno = 0;
            fail("expected free FAT12 cluster is allocated");
        }
    }
}

static int verify_image(const char *path, int growth) {
    size_t image_bytes = 0U;
    uint8_t *image = read_image(path, &image_bytes);
    (void)image_bytes;

    const uint8_t *fat = image + (size_t)RESERVED * BPS;
    size_t root_offset =
        (size_t)(RESERVED + FAT_COUNT * SECTORS_PER_FAT) * BPS;

    if (!growth) {
        verify_file(image, fat, image + root_offset,
                    2U, A_CLUSTERS, 1U);
        verify_file(image, fat, image + root_offset + 32U,
                    402U, B_CLUSTERS, 2U);
        verify_file(image, fat, image + root_offset + 64U,
                    802U, C_CLUSTERS, 3U);
        verify_free_run(fat, 1002U, MAX_CLUSTER);
    } else {
        verify_file(image, fat, image + root_offset,
                    2U, A_CLUSTERS, 1U);
        verify_file(image, fat, image + root_offset + 32U,
                    442U, B_CLUSTERS, 2U);
        verify_file(image, fat, image + root_offset + 64U,
                    882U, C_CLUSTERS, 3U);
        verify_free_run(fat, 402U, 441U);
        verify_free_run(fat, 842U, 881U);
        verify_free_run(fat, 1082U, MAX_CLUSTER);
    }

    free(image);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s create|verify-defrag|verify-growth IMAGE\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "create") == 0)
        return create_image(argv[2]);
    if (strcmp(argv[1], "verify-defrag") == 0)
        return verify_image(argv[2], 0);
    if (strcmp(argv[1], "verify-growth") == 0)
        return verify_image(argv[2], 1);
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return EXIT_FAILURE;
}
