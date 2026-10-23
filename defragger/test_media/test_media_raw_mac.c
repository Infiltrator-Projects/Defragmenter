// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "hfsplus_native.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint8_t data[320];
    size_t bytes;
} MacRecord;
static int node(uint8_t *out, size_t bytes, uint8_t kind, unsigned height, uint32_t next,
                uint32_t previous, const MacRecord *records, unsigned count)
{
    memset(out, 0, bytes);
    infiltratr_store_be32(out, next);
    infiltratr_store_be32(out + 4U, previous);
    out[8U] = kind;
    out[9U] = (uint8_t)height;
    infiltratr_store_be16(out + 10U, (uint16_t)count);
    size_t position = 14U;
    for (unsigned i = 0U; i < count; ++i) {
        if (position > bytes || 2U * (count + 1U) > bytes - position ||
            records[i].bytes > bytes - position - 2U * (count + 1U))
            return -1;
        infiltratr_store_be16(out + bytes - 2U * (i + 1U), (uint16_t)position);
        memcpy(out + position, records[i].data, records[i].bytes);
        position += records[i].bytes;
    }
    infiltratr_store_be16(out + bytes - 2U * (count + 1U), (uint16_t)position);
    return 0;
}
static void header(uint8_t *out, size_t bytes, uint32_t root, uint32_t first, uint32_t last,
                   uint32_t total, unsigned depth, bool plus)
{
    memset(out, 0, bytes);
    out[8U] = 1U;
    infiltratr_store_be16(out + 10U, 3U);
    infiltratr_store_be16(out + 14U, (uint16_t)depth);
    infiltratr_store_be32(out + 16U, root);
    infiltratr_store_be32(out + 20U, first != 0U ? LDTM_RAW_FILES + 1U : 0U);
    infiltratr_store_be32(out + 24U, first);
    infiltratr_store_be32(out + 28U, last);
    infiltratr_store_be16(out + 32U, (uint16_t)bytes);
    infiltratr_store_be16(out + 34U, plus ? 516U : 37U);
    infiltratr_store_be32(out + 36U, total);
    out[51U] = plus ? 0xcfU : 0U;
    infiltratr_store_be16(out + bytes - 2U, 14U);
    infiltratr_store_be16(out + bytes - 4U, 120U);
    infiltratr_store_be16(out + bytes - 6U, 248U);
    infiltratr_store_be16(out + bytes - 8U, (uint16_t)(bytes - 8U));
    for (uint32_t i = 0U; i < total; ++i)
        out[248U + i / 8U] |= (uint8_t)(0x80U >> (i % 8U));
}
static size_t key(uint8_t *out, uint32_t parent, const char *name, bool plus)
{
    const size_t length = strlen(name);
    if (plus) {
        infiltratr_store_be16(out, (uint16_t)(6U + length * 2U));
        infiltratr_store_be32(out + 2U, parent);
        infiltratr_store_be16(out + 6U, (uint16_t)length);
        for (size_t i = 0U; i < length; ++i)
            infiltratr_store_be16(out + 8U + i * 2U, (uint16_t)(unsigned char)name[i]);
        return 8U + length * 2U;
    }
    out[0U] = (uint8_t)(6U + length);
    infiltratr_store_be32(out + 2U, parent);
    out[6U] = (uint8_t)length;
    memcpy(out + 7U, name, length);
    return (8U + length) & ~(size_t)1U;
}
static void plus_fork(uint8_t *out, uint64_t bytes, uint32_t total, const LdtmRawFile *file)
{
    infiltratr_store_be64(out, bytes);
    infiltratr_store_be32(out + 12U, total);
    for (unsigned part = 0U; part < 2U; ++part) {
        infiltratr_store_be32(out + 16U + part * 8U, (uint32_t)file->start[part]);
        infiltratr_store_be32(out + 20U + part * 8U, (uint32_t)file->count[part]);
    }
}
int ldtm_raw_mac(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    const bool plus = spec->creator == LDTM_CREATOR_HFSPLUS;
    const uint32_t unit = plus ? 4096U : 32768U;
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    uint8_t *bitmap = NULL;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB)
            goto done;
        const uint32_t blocks =
            plus ? (uint32_t)(capacity / unit) : (uint32_t)((capacity - unit - 1024U) / unit);
        const uint64_t heap = plus ? 0U : 1U;
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, unit, 256U, &plan) != 0)
            goto done;
        const size_t bitmap_bytes = (blocks + 7U) / 8U;
        bitmap = calloc(bitmap_bytes, 1U);
        if (bitmap == NULL)
            goto done;
        uint64_t used = 0U;
        for (uint32_t block = 0U; block < blocks; ++block) {
            bool allocated = plus ? block == 0U || (block >= 2U && block <= 18U) ||
                                        (block >= 20U && block < 24U) || block == blocks - 1U
                                  : block < 2U;
            for (unsigned file = 0U; !allocated && file < LDTM_RAW_FILES; ++file)
                for (unsigned part = 0U; part < 2U; ++part)
                    if (plan.files[file].count[part] != 0U &&
                        (uint64_t)block + heap >= plan.files[file].start[part] &&
                        (uint64_t)block + heap - plan.files[file].start[part] <
                            plan.files[file].count[part])
                        allocated = true;
            if (allocated) {
                bitmap[block / 8U] |= (uint8_t)(0x80U >> (block % 8U));
                ++used;
            }
        }
        MacRecord records[LDTM_RAW_FILES + 1U];
        memset(records, 0, sizeof(records));
        size_t k = key(records[0].data, 1U, spec->label, plus);
        if (plus) {
            infiltratr_store_be16(records[0].data + k, 1U);
            infiltratr_store_be32(records[0].data + k + 8U, 2U);
            records[0].bytes = k + 88U;
        } else {
            records[0].data[k] = 1U;
            infiltratr_store_be32(records[0].data + k + 6U, 2U);
            records[0].bytes = k + 70U;
        }
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            char name[16];
            ldtm_raw_name(file, name);
            MacRecord *r = &records[file + 1U];
            k = key(r->data, 2U, name, plus);
            uint8_t *data = r->data + k;
            const LdtmRawFile *item = &plan.files[file];
            if (plus) {
                infiltratr_store_be16(data, 2U);
                infiltratr_store_be32(data + 8U, 16U + file);
                plus_fork(data + 88U, item->bytes, (uint32_t)(item->count[0] + item->count[1]),
                          item);
                r->bytes = k + 248U;
            } else {
                data[0U] = 2U;
                infiltratr_store_be32(data + 20U, 16U + file);
                infiltratr_store_be32(data + 26U, (uint32_t)item->bytes);
                infiltratr_store_be32(data + 30U,
                                      (uint32_t)((item->count[0] + item->count[1]) * unit));
                for (unsigned part = 0U; part < 2U; ++part) {
                    infiltratr_store_be16(
                        data + 74U + part * 4U,
                        item->count[part] != 0U ? (uint16_t)(item->start[part] - heap) : 0U);
                    infiltratr_store_be16(data + 76U + part * 4U, (uint16_t)item->count[part]);
                }
                r->bytes = k + 102U;
            }
        }
        if (plus) {
            uint8_t tree[16384], empty[512], vh[512] = {0};
            header(tree, 8192U, 1U, 1U, 1U, 2U, 1U, true);
            if (node(tree + 8192U, 8192U, 0xffU, 1U, 0U, 0U, records, LDTM_RAW_FILES + 1U) != 0)
                goto done;
            header(empty, sizeof(empty), 0U, 0U, 0U, 1U, 0U, true);
            infiltratr_store_be16(vh, 0x482bU);
            infiltratr_store_be16(vh + 2U, 4U);
            infiltratr_store_be32(vh + 4U, 0x100U);
            infiltratr_store_be32(vh + 32U, LDTM_RAW_FILES);
            infiltratr_store_be32(vh + 36U, 1U);
            infiltratr_store_be32(vh + 40U, unit);
            infiltratr_store_be32(vh + 44U, blocks);
            infiltratr_store_be32(vh + 48U, blocks - (uint32_t)used);
            infiltratr_store_be32(vh + 64U, 32U);
            infiltratr_store_be32(vh + 68U, 1U);
            LdtmRawFile bitmap_file = {.start = {2U, 0U}, .count = {16U, 0U}};
            LdtmRawFile extents_file = {.start = {18U, 0U}, .count = {1U, 0U}};
            LdtmRawFile catalog_file = {.start = {20U, 0U}, .count = {4U, 0U}};
            plus_fork(vh + 112U, bitmap_bytes, 16U, &bitmap_file);
            plus_fork(vh + 192U, 512U, 1U, &extents_file);
            plus_fork(vh + 272U, sizeof(tree), 4U, &catalog_file);
            if (ldtm_raw_write(fd, 1024U, vh, sizeof(vh)) != 0 ||
                ldtm_raw_write(fd, capacity - 1024U, vh, sizeof(vh)) != 0 ||
                ldtm_raw_write(fd, 2U * unit, bitmap, bitmap_bytes) != 0 ||
                ldtm_raw_write(fd, 18U * unit, empty, sizeof(empty)) != 0 ||
                ldtm_raw_write(fd, 20U * unit, tree, sizeof(tree)) != 0)
                goto done;
        } else {
            uint8_t tree[4096], empty[512], mdb[512] = {0};
            header(tree, 512U, 1U, 2U, 7U, 8U, 2U, false);
            MacRecord index[6];
            memset(index, 0, sizeof(index));
            unsigned begin = 0U;
            for (unsigned leaf = 0U; leaf < 6U; ++leaf) {
                const unsigned count =
                    LDTM_RAW_FILES + 1U - begin < 3U ? LDTM_RAW_FILES + 1U - begin : 3U;
                const MacRecord *first_record = &records[begin];
                const size_t key_bytes = ((size_t)first_record->data[0U] + 2U) & ~(size_t)1U;
                memcpy(index[leaf].data, first_record->data, key_bytes);
                infiltratr_store_be32(index[leaf].data + key_bytes, leaf + 2U);
                index[leaf].bytes = key_bytes + 4U;
                if (node(tree + (leaf + 2U) * 512U, 512U, 0xffU, 1U, leaf == 5U ? 0U : leaf + 3U,
                         leaf == 0U ? 0U : leaf + 1U, records + begin, count) != 0)
                    goto done;
                begin += count;
            }
            if (node(tree + 512U, 512U, 0U, 2U, 0U, 0U, index, 6U) != 0)
                goto done;
            header(empty, sizeof(empty), 0U, 0U, 0U, 1U, 0U, false);
            infiltratr_store_be16(mdb, 0x4244U);
            infiltratr_store_be16(mdb + 10U, 0x100U);
            infiltratr_store_be16(mdb + 12U, LDTM_RAW_FILES);
            infiltratr_store_be16(mdb + 14U, 3U);
            infiltratr_store_be16(mdb + 18U, (uint16_t)blocks);
            infiltratr_store_be32(mdb + 20U, unit);
            infiltratr_store_be16(mdb + 28U, 64U);
            infiltratr_store_be32(mdb + 30U, 32U);
            infiltratr_store_be16(mdb + 34U, (uint16_t)(blocks - used));
            mdb[36U] = (uint8_t)strlen(spec->label);
            memcpy(mdb + 37U, spec->label, strlen(spec->label));
            infiltratr_store_be32(mdb + 84U, LDTM_RAW_FILES);
            infiltratr_store_be32(mdb + 88U, 1U);
            infiltratr_store_be32(mdb + 130U, sizeof(empty));
            infiltratr_store_be16(mdb + 136U, 1U);
            infiltratr_store_be32(mdb + 146U, sizeof(tree));
            infiltratr_store_be16(mdb + 150U, 1U);
            infiltratr_store_be16(mdb + 152U, 1U);
            if (ldtm_raw_write(fd, 1024U, mdb, sizeof(mdb)) != 0 ||
                ldtm_raw_write(fd, capacity - 1024U, mdb, sizeof(mdb)) != 0 ||
                ldtm_raw_write(fd, 1536U, bitmap, bitmap_bytes) != 0 ||
                ldtm_raw_write(fd, unit, empty, sizeof(empty)) != 0 ||
                ldtm_raw_write(fd, 2U * unit, tree, sizeof(tree)) != 0)
                goto done;
        }
        result = ldtm_raw_write_payload(fd, &plan);
    } else if (plus) {
        HfsPlusVolume volume = {0};
        char *error = NULL;
        if (hfsplus_scan(path, false, &volume, &error) != 0) {
            free(error);
            goto done;
        }
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        bool valid = volume.files.count == LDTM_RAW_FILES;
        for (size_t n = 0U; valid && n < volume.files.count; ++n) {
            const HfsPlusFile *item = &volume.files.items[n];
            if (item->file_id < 16U || item->file_id >= 16U + LDTM_RAW_FILES ||
                item->resource_fork.logical_size != 0U) {
                valid = false;
                break;
            }
            const unsigned file = item->file_id - 16U;
            uint64_t logical = 0U;
            valid = ldtm_raw_check_file(&check, file, item->data_fork.logical_size) == 0;
            for (size_t extent = 0U; valid && logical < item->data_fork.logical_size &&
                                     extent < item->data_fork.extent_count;
                 ++extent) {
                uint64_t take = (uint64_t)item->data_fork.extents[extent].count * volume.block_size;
                if (take > item->data_fork.logical_size - logical)
                    take = item->data_fork.logical_size - logical;
                valid = ldtm_raw_check_data(&check, fd, file,
                                            (uint64_t)item->data_fork.extents[extent].start *
                                                volume.block_size,
                                            logical, take) == 0;
                logical += take;
            }
        }
        result = valid ? ldtm_raw_check_finish(&check) : -1;
        hfsplus_close(&volume);
        free(error);
    } else {
        uint8_t mdb[512], raw[512];
        if (ldtm_raw_read(fd, 1024U, mdb, sizeof(mdb)) != 0 || infiltratr_load_be16(mdb) != 0x4244U)
            goto done;
        const uint32_t block_size = infiltratr_load_be32(mdb + 20U);
        const uint64_t heap = (uint64_t)infiltratr_load_be16(mdb + 28U) * 512U;
        const uint64_t catalog = heap + (uint64_t)infiltratr_load_be16(mdb + 150U) * block_size;
        if (block_size == 0U || ldtm_raw_read(fd, catalog, raw, sizeof(raw)) != 0)
            goto done;
        uint32_t leaf = infiltratr_load_be32(raw + 24U);
        unsigned visits = 0U;
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        while (leaf != 0U) {
            if (++visits > 32U ||
                ldtm_raw_read(fd, catalog + (uint64_t)leaf * 512U, raw, sizeof(raw)) != 0 ||
                raw[8U] != 0xffU)
                goto done;
            const unsigned count = infiltratr_load_be16(raw + 10U);
            if (count > 20U)
                goto done;
            for (unsigned n = 0U; n < count; ++n) {
                const unsigned start = infiltratr_load_be16(raw + 512U - 2U * (n + 1U));
                if (start < 14U || start + 8U >= 512U - 2U * (count + 1U))
                    goto done;
                const unsigned data = start + ((raw[start] + 2U) & ~1U);
                if (data + 102U > 512U - 2U * (count + 1U)) {
                    if (data < 512U && raw[data] == 1U)
                        continue;
                    goto done;
                }
                if (raw[data] != 2U)
                    continue;
                const uint32_t id = infiltratr_load_be32(raw + data + 20U);
                const uint64_t bytes = infiltratr_load_be32(raw + data + 26U);
                if (id < 16U || id >= 16U + LDTM_RAW_FILES ||
                    infiltratr_load_be32(raw + data + 36U) != 0U ||
                    infiltratr_load_be32(raw + data + 40U) != 0U ||
                    ldtm_raw_check_file(&check, id - 16U, bytes) != 0)
                    goto done;
                uint64_t logical = 0U;
                for (unsigned extent = 0U; extent < 3U && logical < bytes; ++extent) {
                    const uint64_t physical =
                        heap +
                        (uint64_t)infiltratr_load_be16(raw + data + 74U + extent * 4U) * block_size;
                    uint64_t take =
                        (uint64_t)infiltratr_load_be16(raw + data + 76U + extent * 4U) * block_size;
                    if (take > bytes - logical)
                        take = bytes - logical;
                    if (take == 0U ||
                        ldtm_raw_check_data(&check, fd, id - 16U, physical, logical, take) != 0)
                        goto done;
                    logical += take;
                }
            }
            leaf = infiltratr_load_be32(raw);
        }
        result = ldtm_raw_check_finish(&check);
    }
done:
    free(bitmap);
    if (close(fd) != 0)
        result = -1;
    return result;
}
