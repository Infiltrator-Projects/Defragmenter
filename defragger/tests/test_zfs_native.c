// SPDX-License-Identifier: GPL-3.0-or-later
#include "zfs_native.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE_BYTES (16U * 1024U * 1024U)
#define LABEL_SIZE (256U * 1024U)
#define UBER_RING_OFFSET (128U * 1024U)
#define UBER_SIZE 1024U

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                          __LINE__, #expr);                                   \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

static void reset_image(int fd)
{
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, IMAGE_BYTES) == 0);
}

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

static void put_le64(uint8_t *data, uint64_t value)
{
    for (unsigned int index = 0U; index < 8U; ++index)
        data[index] = (uint8_t)(value >> (index * 8U));
}

static void put_be64(uint8_t *data, uint64_t value)
{
    for (unsigned int index = 0U; index < 8U; ++index)
        data[7U - index] = (uint8_t)(value >> (index * 8U));
}

static off_t label_base(unsigned label)
{
    if (label < 2U)
        return (off_t)(label * LABEL_SIZE);
    return (off_t)IMAGE_BYTES -
           (off_t)((4U - label) * LABEL_SIZE);
}

static off_t write_uber(int fd, unsigned label, unsigned slot, int big,
                        uint64_t version, uint64_t txg,
                        uint64_t guid_sum, uint64_t timestamp)
{
    uint8_t uber[UBER_SIZE];
    memset(uber, 0, sizeof(uber));
    void (*put64)(uint8_t *, uint64_t) = big ? put_be64 : put_le64;
    put64(uber + 0U, UINT64_C(0x00bab10c));
    put64(uber + 8U, version);
    put64(uber + 16U, txg);
    put64(uber + 24U, guid_sum);
    put64(uber + 32U, timestamp);

    /* MOS root block pointer: one DVA plus ordinary non-embedded properties. */
    const uint64_t root_asize_units = 16U;
    const uint64_t root_vdev = 3U;
    const uint64_t root_offset_units = 0x1234U;
    const uint64_t dva0 = root_asize_units | (root_vdev << 32U);
    const uint64_t dva1 = root_offset_units;
    const uint64_t root_prop =
        UINT64_C(31) |
        (UINT64_C(15) << 16U) |
        (UINT64_C(2) << 32U) |
        (UINT64_C(8) << 40U) |
        (UINT64_C(11) << 48U) |
        (UINT64_C(1) << 56U);
    put64(uber + 40U, dva0);
    put64(uber + 48U, dva1);
    put64(uber + 88U, root_prop);
    put64(uber + 120U, txg);
    const off_t offset = label_base(label) + UBER_RING_OFFSET +
                         (off_t)slot * UBER_SIZE;
    write_all(fd, uber, sizeof(uber), offset);
    return offset;
}

int main(void)
{
    char path[] = "/tmp/linux-defragger-zfs-test-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);

    LdZfsSummary summary;
    char error[256];

    reset_image(fd);
    const off_t first = write_uber(fd, 0U, 3U, 0, 5000U, 10U, 77U, 1000U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.size_bytes == IMAGE_BYTES);
    CHECK(summary.uberblock_magic_offset == (uint64_t)first);
    CHECK(summary.uberblock_txg == 10U);
    CHECK(summary.uberblock_version == 5000U);
    CHECK(summary.uberblock_guid_sum == 77U);
    CHECK(summary.uberblock_timestamp == 1000U);
    CHECK(summary.label_index == 0U);
    CHECK(summary.uberblock_slot == 3U);
    CHECK(summary.candidate_uberblocks == 1U);
    CHECK(summary.byte_order == LD_ZFS_BYTE_ORDER_LITTLE);
    CHECK(summary.root_vdev == 3U);
    CHECK(summary.root_offset == (UINT64_C(0x1234) << 9U));
    CHECK(summary.root_asize == (UINT64_C(16) << 9U));
    CHECK(summary.root_lsize == (UINT64_C(32) << 9U));
    CHECK(summary.root_psize == (UINT64_C(16) << 9U));
    CHECK(summary.root_logical_birth == 10U);
    CHECK(summary.root_compression == 2U);
    CHECK(summary.root_checksum == 8U);
    CHECK(summary.root_type == 11U);
    CHECK(summary.root_level == 1U);
    CHECK(!summary.root_embedded);
    CHECK(strcmp(zfs_byte_order_name(&summary), "little") == 0);
    CHECK(zfs_probe(path));

    const off_t best = write_uber(fd, 3U, 127U, 1, 5000U, 42U, 99U, 2000U);
    (void)write_uber(fd, 1U, 8U, 0, 28U, 20U, 88U, 1500U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.uberblock_magic_offset == (uint64_t)best);
    CHECK(summary.uberblock_txg == 42U);
    CHECK(summary.uberblock_version == 5000U);
    CHECK(summary.uberblock_guid_sum == 99U);
    CHECK(summary.uberblock_timestamp == 2000U);
    CHECK(summary.label_index == 3U);
    CHECK(summary.uberblock_slot == 127U);
    CHECK(summary.candidate_uberblocks == 3U);
    CHECK(summary.byte_order == LD_ZFS_BYTE_ORDER_BIG);
    CHECK(summary.root_logical_birth == 42U);
    CHECK(strcmp(zfs_byte_order_name(&summary), "big") == 0);

    /* Same TXG: the newer timestamp is the deterministic winner. */
    const off_t newer = write_uber(fd, 2U, 9U, 0, 5000U, 42U, 111U, 3000U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.uberblock_magic_offset == (uint64_t)newer);
    CHECK(summary.uberblock_timestamp == 3000U);
    CHECK(summary.label_index == 2U);
    CHECK(summary.uberblock_slot == 9U);
    CHECK(summary.candidate_uberblocks == 4U);

    /*
     * A magic-shaped value outside a legal label uberblock ring is not a ZFS
     * member.  The old scanner incorrectly accepted this kind of false positive.
     */
    reset_image(fd);
    static const uint8_t bare_magic[8] =
        {0x0cU, 0xb1U, 0xbaU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    write_all(fd, bare_magic, sizeof(bare_magic), 6U * 1024U * 1024U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) != 0);
    CHECK(!zfs_probe(path));
    CHECK(strstr(error, "committed label uberblock") != NULL);

    /* A ring entry with magic but no committed TXG is also rejected. */
    reset_image(fd);
    (void)write_uber(fd, 0U, 0U, 0, 5000U, 0U, 1U, 1U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) != 0);

    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
    (void)puts("ZFS native four-label uberblock parser tests passed");
    return 0;
}
