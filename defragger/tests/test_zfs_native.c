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
#define DATA_TYPE_BOOLEAN 1U
#define DATA_TYPE_UINT64 8U
#define DATA_TYPE_STRING 9U
#define DATA_TYPE_NVLIST 19U
#define DATA_TYPE_NVLIST_ARRAY 20U
#define VDEV_DATA_START (4U * 1024U * 1024U)
#define MOS_ROOT_LOGICAL_OFFSET (2U * 1024U * 1024U)
#define META_DNODE_LOGICAL_OFFSET (3U * 1024U * 1024U)
#define METASLAB_ARRAY_LOGICAL_OFFSET (4U * 1024U * 1024U)
#define SPACE_MAP_LOGICAL_OFFSET (5U * 1024U * 1024U)
#define DATASET_ROOT_LOGICAL_OFFSET (6U * 1024U * 1024U)
#define DATASET_DNODE_LOGICAL_OFFSET (7U * 1024U * 1024U)

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

static off_t label_base(unsigned label);

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

static void xw_boolean_pair(XdrWriter *writer, const char *name)
{
    const size_t start = xw_pair_begin(writer, name, DATA_TYPE_BOOLEAN, 0U);
    xw_pair_end(writer, start);
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

static void write_label_config(int fd, unsigned label,
                               const char *mos_feature)
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

    if (mos_feature != NULL) {
        size_t features =
            xw_pair_begin(&writer, "features_for_read", DATA_TYPE_NVLIST, 1U);
        xw_nvlist_begin(&writer);
        xw_boolean_pair(&writer, mos_feature);
        xw_nvlist_end(&writer);
        xw_pair_end(&writer, features);
    }

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
    xw_uint64_pair(&writer, "metaslab_array", 2U);
    xw_uint64_pair(&writer, "metaslab_shift", 22U);
    xw_uint64_pair(&writer, "asize", UINT64_C(8) * 1024U * 1024U);
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

static void put16(uint8_t *data, uint16_t value, int big)
{
    if (big) {
        data[0] = (uint8_t)(value >> 8U);
        data[1] = (uint8_t)value;
    } else {
        data[0] = (uint8_t)value;
        data[1] = (uint8_t)(value >> 8U);
    }
}

static void fletcher4(const uint8_t *data, size_t length, int big,
                      uint64_t words[4])
{
    CHECK((length & 3U) == 0U);
    uint64_t a = 0U, b = 0U, csum = 0U, d = 0U;
    for (size_t offset = 0U; offset < length; offset += 4U) {
        uint32_t value;
        if (big) {
            value = ((uint32_t)data[offset] << 24U) |
                    ((uint32_t)data[offset + 1U] << 16U) |
                    ((uint32_t)data[offset + 2U] << 8U) |
                    (uint32_t)data[offset + 3U];
        } else {
            value = (uint32_t)data[offset] |
                    ((uint32_t)data[offset + 1U] << 8U) |
                    ((uint32_t)data[offset + 2U] << 16U) |
                    ((uint32_t)data[offset + 3U] << 24U);
        }
        a += value;
        b += a;
        csum += b;
        d += csum;
    }
    words[0] = a;
    words[1] = b;
    words[2] = csum;
    words[3] = d;
}

static void encode_bp(uint8_t bp[128], int big, uint64_t logical_offset,
                      uint32_t type, const uint8_t *data, size_t data_size)
{
    CHECK(data_size != 0U && (data_size % 512U) == 0U);
    memset(bp, 0, 128U);
    void (*put64)(uint8_t *, uint64_t) = big ? put_be64 : put_le64;
    const uint64_t sectors = data_size / 512U;
    const uint64_t word0 = sectors | (UINT64_C(3) << 32U);
    const uint64_t word1 = logical_offset >> 9U;
    const uint64_t prop =
        (sectors - 1U) |
        ((sectors - 1U) << 16U) |
        (UINT64_C(2) << 32U) |
        (UINT64_C(7) << 40U) |
        ((uint64_t)type << 48U) |
        (big ? 0U : (UINT64_C(1) << 63U));
    put64(bp + 0U, word0);
    put64(bp + 8U, word1);
    put64(bp + 48U, prop);
    put64(bp + 80U, 10U);
    uint64_t checksum[4];
    fletcher4(data, data_size, big, checksum);
    for (size_t index = 0U; index < 4U; ++index)
        put64(bp + 96U + index * 8U, checksum[index]);
}

static void make_dnode(uint8_t dnode[512], int big, uint8_t type,
                       uint8_t bonus_type, uint16_t data_sectors,
                       uint16_t bonus_length, uint64_t max_block_id,
                       const uint8_t bp[128])
{
    memset(dnode, 0, 512U);
    void (*put64)(uint8_t *, uint64_t) = big ? put_be64 : put_le64;
    dnode[0] = type;
    dnode[1] = 14U;
    dnode[2] = 1U;
    dnode[3] = 1U;
    dnode[4] = bonus_type;
    dnode[5] = 7U;
    dnode[6] = 2U;
    put16(dnode + 8U, data_sectors, big);
    put16(dnode + 10U, bonus_length, big);
    put64(dnode + 16U, max_block_id);
    put64(dnode + 24U, (uint64_t)data_sectors << 9U);
    memcpy(dnode + 64U, bp, 128U);
}

static void write_exact_fixture(int fd)
{
    uint8_t space_map_data[4096];
    memset(space_map_data, 0, sizeof(space_map_data));
    const uint64_t alloc_entry = UINT64_C(15);
    const uint64_t free_entry =
        (UINT64_C(4) << 16U) | (UINT64_C(1) << 15U) | UINT64_C(3);
    put_le64(space_map_data + 0U, alloc_entry);
    put_le64(space_map_data + 8U, free_entry);
    write_all(fd, space_map_data, sizeof(space_map_data),
              (off_t)(VDEV_DATA_START + SPACE_MAP_LOGICAL_OFFSET));

    uint8_t space_map_bp[128];
    encode_bp(space_map_bp, 0, SPACE_MAP_LOGICAL_OFFSET, 8U,
              space_map_data, sizeof(space_map_data));

    uint8_t metaslab_array_data[4096];
    memset(metaslab_array_data, 0, sizeof(metaslab_array_data));
    put_le64(metaslab_array_data + 0U, 3U);
    put_le64(metaslab_array_data + 8U, 0U);
    write_all(fd, metaslab_array_data, sizeof(metaslab_array_data),
              (off_t)(VDEV_DATA_START + METASLAB_ARRAY_LOGICAL_OFFSET));

    uint8_t metaslab_array_bp[128];
    encode_bp(metaslab_array_bp, 0, METASLAB_ARRAY_LOGICAL_OFFSET, 2U,
              metaslab_array_data, sizeof(metaslab_array_data));

    uint8_t dummy_file_data[4096];
    memset(dummy_file_data, 0, sizeof(dummy_file_data));
    uint8_t file_bp0[128];
    uint8_t file_bp1[128];
    encode_bp(file_bp0, 0, 0U, 19U,
              dummy_file_data, sizeof(dummy_file_data));
    encode_bp(file_bp1, 0, 32U * 1024U, 19U,
              dummy_file_data, sizeof(dummy_file_data));

    uint8_t dataset_dnodes[16384];
    memset(dataset_dnodes, 0, sizeof(dataset_dnodes));
    make_dnode(dataset_dnodes + 512U, 0, 19U, 0U, 8U, 0U, 1U,
               file_bp0);
    dataset_dnodes[512U + 3U] = 2U;
    memcpy(dataset_dnodes + 512U + 192U, file_bp1, sizeof(file_bp1));
    write_all(fd, dataset_dnodes, sizeof(dataset_dnodes),
              (off_t)(VDEV_DATA_START + DATASET_DNODE_LOGICAL_OFFSET));

    uint8_t dataset_meta_bp[128];
    encode_bp(dataset_meta_bp, 0, DATASET_DNODE_LOGICAL_OFFSET, 10U,
              dataset_dnodes, sizeof(dataset_dnodes));

    uint8_t dataset_objset[4096];
    memset(dataset_objset, 0, sizeof(dataset_objset));
    make_dnode(dataset_objset, 0, 10U, 0U, 32U, 0U, 0U,
               dataset_meta_bp);
    write_all(fd, dataset_objset, sizeof(dataset_objset),
              (off_t)(VDEV_DATA_START + DATASET_ROOT_LOGICAL_OFFSET));

    uint8_t dataset_root_bp[128];
    encode_bp(dataset_root_bp, 0, DATASET_ROOT_LOGICAL_OFFSET, 11U,
              dataset_objset, sizeof(dataset_objset));

    uint8_t dnode_block[16384];
    memset(dnode_block, 0, sizeof(dnode_block));
    make_dnode(dnode_block + 2U * 512U, 0, 2U, 0U, 8U, 0U, 0U,
               metaslab_array_bp);
    make_dnode(dnode_block + 3U * 512U, 0, 8U, 7U, 8U, 24U, 0U,
               space_map_bp);
    put_le64(dnode_block + 3U * 512U + 192U + 8U, 16U);
    put_le64(dnode_block + 3U * 512U + 192U + 16U,
             UINT64_C(12) * 4096U);

    uint8_t dataset_hole_bp[128];
    memset(dataset_hole_bp, 0, sizeof(dataset_hole_bp));
    make_dnode(dnode_block + 4U * 512U, 0, 16U, 16U, 8U, 256U, 0U,
               dataset_hole_bp);
    memcpy(dnode_block + 4U * 512U + 192U + 128U,
           dataset_root_bp, sizeof(dataset_root_bp));

    write_all(fd, dnode_block, sizeof(dnode_block),
              (off_t)(VDEV_DATA_START + META_DNODE_LOGICAL_OFFSET));

    uint8_t meta_bp[128];
    encode_bp(meta_bp, 0, META_DNODE_LOGICAL_OFFSET, 10U,
              dnode_block, sizeof(dnode_block));

    uint8_t mos[4096];
    memset(mos, 0, sizeof(mos));
    make_dnode(mos, 0, 10U, 0U, 32U, 0U, 0U, meta_bp);
    write_all(fd, mos, sizeof(mos),
              (off_t)(VDEV_DATA_START + MOS_ROOT_LOGICAL_OFFSET));
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
    const uint64_t root_asize_units = 8U;
    const uint64_t root_vdev = 3U;
    const uint64_t root_offset_units = MOS_ROOT_LOGICAL_OFFSET >> 9U;
    const uint64_t dva0 = root_asize_units | (root_vdev << 32U);
    const uint64_t dva1 = root_offset_units;
    const uint64_t root_prop =
        UINT64_C(7) |
        (UINT64_C(7) << 16U) |
        (UINT64_C(2) << 32U) |
        (UINT64_C(7) << 40U) |
        (UINT64_C(11) << 48U) |
        (big ? 0U : (UINT64_C(1) << 63U));
    put64(uber + 40U, dva0);
    put64(uber + 48U, dva1);
    put64(uber + 88U, root_prop);
    put64(uber + 120U, txg);
    if (!big) {
        uint8_t mos[4096];
        const ssize_t count = pread(fd, mos, sizeof(mos),
            (off_t)(VDEV_DATA_START + MOS_ROOT_LOGICAL_OFFSET));
        CHECK(count == (ssize_t)sizeof(mos));
        uint64_t checksum[4];
        fletcher4(mos, sizeof(mos), 0, checksum);
        for (size_t index = 0U; index < 4U; ++index)
            put64(uber + 40U + 96U + index * 8U, checksum[index]);
    }
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
    write_label_config(fd, 0U, NULL);
    write_exact_fixture(fd);
    const off_t first = write_uber(fd, 0U, 3U, 0, 28U, 10U, 77U, 1000U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.size_bytes == IMAGE_BYTES);
    CHECK(summary.uberblock_magic_offset == (uint64_t)first);
    CHECK(summary.uberblock_txg == 10U);
    CHECK(summary.uberblock_version == 28U);
    CHECK(summary.uberblock_guid_sum == 77U);
    CHECK(summary.uberblock_timestamp == 1000U);
    CHECK(summary.label_index == 0U);
    CHECK(summary.uberblock_slot == 3U);
    CHECK(summary.candidate_uberblocks == 1U);
    CHECK(summary.byte_order == LD_ZFS_BYTE_ORDER_LITTLE);
    CHECK(summary.root_vdev == 3U);
    CHECK(summary.root_offset == MOS_ROOT_LOGICAL_OFFSET);
    CHECK(summary.root_asize == (UINT64_C(8) << 9U));
    CHECK(summary.root_lsize == 4096U);
    CHECK(summary.root_psize == 4096U);
    CHECK(summary.root_logical_birth == 10U);
    CHECK(summary.root_compression == 2U);
    CHECK(summary.root_checksum == 7U);
    CHECK(summary.root_type == 11U);
    CHECK(summary.root_level == 0U);
    CHECK(!summary.root_embedded);
    CHECK(summary.config_known);
    CHECK(summary.single_leaf_supported);
    CHECK(summary.pool_guid == UINT64_C(0x1111222233334444));
    CHECK(summary.leaf_guid == UINT64_C(0x5555666677778888));
    CHECK(summary.top_guid == UINT64_C(0x9999aaaabbbbcccc));
    CHECK(summary.metaslab_vdevs == 1U);
    CHECK(summary.top_vdev_id == 3U);
    CHECK(summary.ashift == 12U);
    CHECK(summary.metaslab_array == 2U);
    CHECK(summary.metaslab_shift == 22U);
    CHECK(summary.top_vdev_asize == UINT64_C(8) * 1024U * 1024U);
    CHECK(strcmp(summary.top_vdev_type, "disk") == 0);
    CHECK(strcmp(zfs_byte_order_name(&summary), "little") == 0);
    CHECK(zfs_probe(path));

    LdZfsAnalysis analysis;
    CHECK(zfs_analyse_exact(path, &analysis, error, sizeof(error)) == 0);
    CHECK(analysis.exact_allocation);
    CHECK(analysis.exact_fragmentation);
    CHECK(analysis.size_bytes == IMAGE_BYTES);
    CHECK(analysis.free_bytes ==
          UINT64_C(8) * 1024U * 1024U - UINT64_C(12) * 4096U);
    CHECK(analysis.used_bytes == analysis.size_bytes - analysis.free_bytes);
    CHECK(analysis.unknown_bytes == 0U);
    CHECK(analysis.allocated_extents == 2U);
    CHECK(analysis.files_seen == 1U);
    CHECK(analysis.fragmented_files == 1U);
    CHECK(analysis.fragmented_bytes == 8192U);
    CHECK(analysis.range_count == 7U);
    zfs_analysis_destroy(&analysis);


    /*
     * Feature-flag pools use the same MOS/metaslab machinery when every
     * read-critical MOS feature is one the bounded reader understands.
     */
    reset_image(fd);
    write_label_config(fd, 0U, "com.delphix:hole_birth");
    write_exact_fixture(fd);
    (void)write_uber(fd, 0U, 11U, 0, 5000U, 50U, 123U, 4000U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.uberblock_version == 5000U);
    CHECK(summary.mos_features_present);
    CHECK(summary.mos_features_supported);
    CHECK(summary.mos_feature_count == 1U);
    CHECK(summary.single_leaf_supported);
    CHECK(zfs_analyse_exact(path, &analysis, error, sizeof(error)) == 0);
    CHECK(analysis.exact_allocation);
    CHECK(analysis.exact_fragmentation);
    CHECK(analysis.fragmented_files == 1U);
    zfs_analysis_destroy(&analysis);

    /*
     * Unknown MOS-format features must not be guessed. Identification remains
     * available, but exact traversal is refused.
     */
    reset_image(fd);
    write_label_config(fd, 0U, "com.example:future_mos");
    write_exact_fixture(fd);
    (void)write_uber(fd, 0U, 12U, 0, 5000U, 51U, 124U, 4001U);
    CHECK(zfs_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.mos_features_present);
    CHECK(!summary.mos_features_supported);
    CHECK(strcmp(summary.unsupported_mos_feature,
                 "com.example:future_mos") == 0);
    CHECK(!summary.single_leaf_supported);
    CHECK(zfs_analyse_exact(path, &analysis, error, sizeof(error)) != 0);

    reset_image(fd);
    write_label_config(fd, 0U, NULL);
    write_exact_fixture(fd);
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
