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
#define VDEV_PHYS_OFFSET (16U * 1024U)
#define VDEV_PHYS_SIZE (112U * 1024U)
#define DATA_TYPE_UINT64 8U
#define DATA_TYPE_STRING 9U
#define DATA_TYPE_NVLIST 19U
#define DATA_TYPE_NVLIST_ARRAY 20U

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

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t position;
} XdrWriter;

static void xw_u32(XdrWriter *writer, uint32_t value)
{
    CHECK(writer->position + 4U <= writer->capacity);
    writer->data[writer->position++] = (uint8_t)(value >> 24U);
    writer->data[writer->position++] = (uint8_t)(value >> 16U);
    writer->data[writer->position++] = (uint8_t)(value >> 8U);
    writer->data[writer->position++] = (uint8_t)value;
}

static void xw_u64(XdrWriter *writer, uint64_t value)
{
    xw_u32(writer, (uint32_t)(value >> 32U));
    xw_u32(writer, (uint32_t)value);
}

static void xw_string(XdrWriter *writer, const char *text)
{
    const size_t length = strlen(text);
    CHECK(length <= UINT32_MAX);
    xw_u32(writer, (uint32_t)length);
    CHECK(writer->position + ((length + 3U) & ~(size_t)3U) <= writer->capacity);
    memcpy(writer->data + writer->position, text, length);
    writer->position += length;
    while ((writer->position & 3U) != 0U)
        writer->data[writer->position++] = 0U;
}

static size_t xw_pair_begin(XdrWriter *writer, const char *name,
                            uint32_t type, uint32_t elements)
{
    const size_t start = writer->position;
    xw_u32(writer, 0U);
    xw_u32(writer, 0U);
    xw_string(writer, name);
    xw_u32(writer, type);
    xw_u32(writer, elements);
    return start;
}

static void xw_pair_end(XdrWriter *writer, size_t start)
{
    const size_t encoded = writer->position - start;
    CHECK(encoded <= UINT32_MAX);
    const uint32_t value = (uint32_t)encoded;
    writer->data[start + 0U] = (uint8_t)(value >> 24U);
    writer->data[start + 1U] = (uint8_t)(value >> 16U);
    writer->data[start + 2U] = (uint8_t)(value >> 8U);
    writer->data[start + 3U] = (uint8_t)value;
    writer->data[start + 4U] = writer->data[start + 0U];
    writer->data[start + 5U] = writer->data[start + 1U];
    writer->data[start + 6U] = writer->data[start + 2U];
    writer->data[start + 7U] = writer->data[start + 3U];
}

static void xw_nvlist_begin(XdrWriter *writer)
{
    xw_u32(writer, 0U);
    xw_u32(writer, 0U);
}

static void xw_nvlist_end(XdrWriter *writer)
{
    xw_u32(writer, 0U);
    xw_u32(writer, 0U);
}

static void xw_uint64_pair(XdrWriter *writer, const char *name, uint64_t value)
{
    const size_t start = xw_pair_begin(writer, name, DATA_TYPE_UINT64, 1U);
    xw_u64(writer, value);
    xw_pair_end(writer, start);
}

static void xw_string_pair(XdrWriter *writer, const char *name, const char *value)
{
    const size_t start = xw_pair_begin(writer, name, DATA_TYPE_STRING, 1U);
    xw_string(writer, value);
    xw_pair_end(writer, start);
}

static void write_label_config(int fd, unsigned label)
{
    uint8_t config[VDEV_PHYS_SIZE];
    memset(config, 0, sizeof(config));
    config[0] = 1U; /* NV_ENCODE_XDR */
    XdrWriter writer = {
        .data = config,
        .capacity = sizeof(config),
        .position = 4U,
    };
    xw_nvlist_begin(&writer);
    xw_uint64_pair(&writer, "pool_guid", UINT64_C(0x1111222233334444));
    xw_uint64_pair(&writer, "guid", UINT64_C(0x5555666677778888));
    xw_uint64_pair(&writer, "top_guid", UINT64_C(0x9999aaaabbbbcccc));

    size_t vdev_tree = xw_pair_begin(&writer, "vdev_tree", DATA_TYPE_NVLIST, 1U);
    xw_nvlist_begin(&writer);
    xw_string_pair(&writer, "type", "root");
    size_t children =
        xw_pair_begin(&writer, "children", DATA_TYPE_NVLIST_ARRAY, 1U);
    xw_nvlist_begin(&writer);
    xw_string_pair(&writer, "type", "disk");
    xw_uint64_pair(&writer, "id", 3U);
    xw_uint64_pair(&writer, "guid", UINT64_C(0x5555666677778888));
    xw_uint64_pair(&writer, "ashift", 12U);
    xw_uint64_pair(&writer, "metaslab_array", 42U);
    xw_uint64_pair(&writer, "metaslab_shift", 29U);
    xw_nvlist_end(&writer);
    xw_pair_end(&writer, children);
    xw_nvlist_end(&writer);
    xw_pair_end(&writer, vdev_tree);
    xw_nvlist_end(&writer);

    write_all(fd, config, sizeof(config),
              label_base(label) + VDEV_PHYS_OFFSET);
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
    write_label_config(fd, 0U);
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
    CHECK(summary.config_known);
    CHECK(summary.pool_guid == UINT64_C(0x1111222233334444));
    CHECK(summary.leaf_guid == UINT64_C(0x5555666677778888));
    CHECK(summary.top_guid == UINT64_C(0x9999aaaabbbbcccc));
    CHECK(summary.metaslab_vdevs == 1U);
    CHECK(summary.top_vdev_id == 3U);
    CHECK(summary.ashift == 12U);
    CHECK(summary.metaslab_array == 42U);
    CHECK(summary.metaslab_shift == 29U);
    CHECK(strcmp(summary.top_vdev_type, "disk") == 0);
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
