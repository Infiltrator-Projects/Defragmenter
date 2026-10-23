// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "ntfs_native.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NT_UNIT 4096U
#define NT_RECORD 1024U
#define NT_MFT 4U
#define NT_RECORDS 64U

static size_t resident(uint8_t *out, uint32_t type, const void *value, size_t bytes)
{
    const size_t length = (24U + bytes + 7U) & ~(size_t)7U;
    memset(out, 0, length);
    infiltratr_store_le32(out, type);
    infiltratr_store_le32(out + 4U, (uint32_t)length);
    infiltratr_store_le32(out + 16U, (uint32_t)bytes);
    infiltratr_store_le16(out + 20U, 24U);
    if (bytes != 0U)
        memcpy(out + 24U, value, bytes);
    return length;
}
static size_t nonresident(uint8_t *out, uint32_t type, const LdtmRawFile *file)
{
    /* Fixed-width signed deltas leave ample rewrite capacity for the writer's
     * one-run destination without borrowing its runlist encoder. */
    memset(out, 0, 112U);
    infiltratr_store_le32(out, type);
    infiltratr_store_le32(out + 4U, 112U);
    out[8U] = 1U;
    const uint64_t clusters = file->count[0] + file->count[1];
    infiltratr_store_le64(out + 24U, clusters - 1U);
    infiltratr_store_le16(out + 32U, 64U);
    infiltratr_store_le64(out + 40U, clusters * NT_UNIT);
    infiltratr_store_le64(out + 48U, file->bytes);
    infiltratr_store_le64(out + 56U, file->bytes);
    size_t cursor = 64U;
    uint64_t previous = 0U;
    for (unsigned part = 0U; part < 2U; ++part) {
        if (file->count[part] == 0U)
            continue;
        out[cursor++] = 0x44U;
        infiltratr_store_le32(out + cursor, (uint32_t)file->count[part]);
        cursor += 4U;
        infiltratr_store_le32(out + cursor, (uint32_t)(file->start[part] - previous));
        cursor += 4U;
        previous = file->start[part];
    }
    return 112U;
}
static void record_begin(uint8_t out[NT_RECORD], unsigned number, bool directory)
{
    memset(out, 0, NT_RECORD);
    memcpy(out, "FILE", 4U);
    infiltratr_store_le16(out + 4U, 48U);
    infiltratr_store_le16(out + 6U, 3U);
    infiltratr_store_le16(out + 16U, 1U);
    infiltratr_store_le16(out + 18U, 1U);
    infiltratr_store_le16(out + 20U, 56U);
    infiltratr_store_le16(out + 22U, directory ? 3U : 1U);
    infiltratr_store_le32(out + 28U, NT_RECORD);
    infiltratr_store_le32(out + 44U, number);
}
static void fixups(uint8_t *out, size_t bytes, unsigned offset)
{
    const unsigned sectors = (unsigned)(bytes / 512U);
    infiltratr_store_le16(out + offset, 0x5aa5U);
    for (unsigned i = 1U; i <= sectors; ++i) {
        memcpy(out + offset + i * 2U, out + i * 512U - 2U, 2U);
        infiltratr_store_le16(out + i * 512U - 2U, 0x5aa5U);
    }
}
static int record_write(int fd, uint8_t out[NT_RECORD], unsigned number, size_t end)
{
    if (end > NT_RECORD - 8U)
        return -1;
    infiltratr_store_le32(out + end, UINT32_MAX);
    infiltratr_store_le32(out + 24U, (uint32_t)(end + 8U));
    fixups(out, NT_RECORD, 48U);
    if (ldtm_raw_write(fd, NT_MFT * NT_UNIT + (uint64_t)number * NT_RECORD, out, NT_RECORD) != 0)
        return -1;
    return number < 4U
               ? ldtm_raw_write(fd, 96U * NT_UNIT + (uint64_t)number * NT_RECORD, out, NT_RECORD)
               : 0;
}
static size_t filename(uint8_t out[96], unsigned file, uint64_t bytes)
{
    memset(out, 0, 96U);
    infiltratr_store_le64(out, UINT64_C(0x1000000000005));
    infiltratr_store_le64(out + 40U, (bytes + NT_UNIT - 1U) / NT_UNIT * NT_UNIT);
    infiltratr_store_le64(out + 48U, bytes);
    char name[16];
    ldtm_raw_name(file, name);
    const size_t length = strlen(name);
    out[64U] = (uint8_t)length;
    out[65U] = 3U;
    for (size_t n = 0U; n < length; ++n)
        infiltratr_store_le16(out + 66U + n * 2U, (uint16_t)(unsigned char)name[n]);
    return 66U + length * 2U;
}

int ldtm_raw_ntfs(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB || capacity / NT_UNIT != 524288U)
            goto done;
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, NT_UNIT, 4096U, &plan) != 0)
            goto done;
        uint8_t boot[512] = {0}, raw[NT_RECORD], bitmap[65536] = {0};
        memcpy(boot, "\xeb\x52\x90NTFS    ", 11U);
        infiltratr_store_le16(boot + 11U, 512U);
        boot[13U] = 8U;
        boot[21U] = 0xf8U;
        infiltratr_store_le64(boot + 40U, capacity / 512U);
        infiltratr_store_le64(boot + 48U, NT_MFT);
        infiltratr_store_le64(boot + 56U, 96U);
        boot[64U] = 0xf6U;
        boot[68U] = 1U;
        memcpy(boot + 72U, "LDTMNTFS", 8U);
        infiltratr_store_le16(boot + 510U, 0xaa55U);
        for (uint64_t block = 0U; block < capacity / NT_UNIT; ++block) {
            bool used = block < 20U || (block >= 64U && block <= 80U) || block == 96U ||
                        block == capacity / NT_UNIT - 1U;
            for (unsigned file = 0U; !used && file < LDTM_RAW_FILES; ++file)
                for (unsigned part = 0U; part < 2U; ++part)
                    if (plan.files[file].count[part] != 0U &&
                        block >= plan.files[file].start[part] &&
                        block - plan.files[file].start[part] < plan.files[file].count[part])
                        used = true;
            if (used)
                bitmap[block / 8U] |= (uint8_t)(1U << (block % 8U));
        }
        LdtmRawFile special = {
            .bytes = NT_RECORDS * NT_RECORD, .start = {NT_MFT, 0U}, .count = {16U, 0U}};
        record_begin(raw, 0U, false);
        size_t end = 56U + nonresident(raw + 56U, 0x80U, &special);
        uint8_t mft_bitmap[8] = {0};
        for (unsigned n = 0U; n < 40U; ++n)
            if (n == 0U || n == 1U || n == 3U || n == 5U || n == 6U || n >= 24U)
                mft_bitmap[n / 8U] |= (uint8_t)(1U << (n % 8U));
        end += resident(raw + end, 0xb0U, mft_bitmap, sizeof(mft_bitmap));
        if (record_write(fd, raw, 0U, end) != 0)
            goto done;
        special = (LdtmRawFile){.bytes = 4096U, .start = {96U, 0U}, .count = {1U, 0U}};
        record_begin(raw, 1U, false);
        end = 56U + nonresident(raw + 56U, 0x80U, &special);
        if (record_write(fd, raw, 1U, end) != 0)
            goto done;
        uint8_t info[12] = {0};
        info[8U] = 3U;
        info[9U] = 1U;
        record_begin(raw, 3U, false);
        end = 56U + resident(raw + 56U, 0x70U, info, sizeof(info));
        if (record_write(fd, raw, 3U, end) != 0)
            goto done;
        special = (LdtmRawFile){.bytes = sizeof(bitmap), .start = {64U, 0U}, .count = {16U, 0U}};
        record_begin(raw, 6U, false);
        end = 56U + nonresident(raw + 56U, 0x80U, &special);
        if (record_write(fd, raw, 6U, end) != 0)
            goto done;
        uint8_t index[4096] = {0}, root[56] = {0};
        memcpy(index, "INDX", 4U);
        infiltratr_store_le16(index + 4U, 40U);
        infiltratr_store_le16(index + 6U, 9U);
        infiltratr_store_le32(index + 24U, 40U);
        infiltratr_store_le32(index + 32U, 4096U - 24U);
        size_t entry = 64U;
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            uint8_t name[96];
            const size_t length = filename(name, file, plan.files[file].bytes);
            const size_t entry_length = (16U + length + 7U) & ~(size_t)7U;
            infiltratr_store_le64(index + entry, UINT64_C(0x1000000000000) + 24U + file);
            infiltratr_store_le16(index + entry + 8U, (uint16_t)entry_length);
            infiltratr_store_le16(index + entry + 10U, (uint16_t)length);
            memcpy(index + entry + 16U, name, length);
            entry += entry_length;
            record_begin(raw, 24U + file, false);
            end = 56U + resident(raw + 56U, 0x30U, name, length);
            if (plan.files[file].bytes == 0U)
                end += resident(raw + end, 0x80U, NULL, 0U);
            else
                end += nonresident(raw + end, 0x80U, &plan.files[file]);
            if (record_write(fd, raw, 24U + file, end) != 0)
                goto done;
        }
        infiltratr_store_le16(index + entry + 8U, 16U);
        infiltratr_store_le16(index + entry + 12U, 2U);
        infiltratr_store_le32(index + 28U, (uint32_t)(entry + 16U - 24U));
        fixups(index, sizeof(index), 40U);
        infiltratr_store_le32(root, 0x30U);
        infiltratr_store_le32(root + 4U, 1U);
        infiltratr_store_le32(root + 8U, 4096U);
        root[12U] = 1U;
        infiltratr_store_le32(root + 16U, 16U);
        infiltratr_store_le32(root + 20U, 40U);
        infiltratr_store_le32(root + 24U, 40U);
        root[28U] = 1U;
        infiltratr_store_le16(root + 40U, 24U);
        infiltratr_store_le16(root + 44U, 3U);
        record_begin(raw, 5U, true);
        end = 56U + resident(raw + 56U, 0x90U, root, sizeof(root));
        special = (LdtmRawFile){.bytes = 4096U, .start = {80U, 0U}, .count = {1U, 0U}};
        end += nonresident(raw + end, 0xa0U, &special);
        const uint8_t index_bitmap = 1U;
        end += resident(raw + end, 0xb0U, &index_bitmap, 1U);
        if (record_write(fd, raw, 5U, end) != 0 ||
            ldtm_raw_write(fd, 0U, boot, sizeof(boot)) != 0 ||
            ldtm_raw_write(fd, capacity - 512U, boot, sizeof(boot)) != 0 ||
            ldtm_raw_write(fd, 64U * NT_UNIT, bitmap, sizeof(bitmap)) != 0 ||
            ldtm_raw_write(fd, 80U * NT_UNIT, index, sizeof(index)) != 0)
            goto done;
        result = ldtm_raw_write_payload(fd, &plan);
    } else {
        NtfsVolume volume = {.fd = -1};
        NtfsLayout layout = {0};
        char *error = NULL;
        if (ntfs_open_volume(path, false, &volume, &error) != 0) {
            free(error);
            goto done;
        }
        bool valid = ntfs_read_layout(&volume, false, &layout, &error) == 0 &&
                     volume.cluster_size == NT_UNIT;
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        for (unsigned file = 0U; valid && file < LDTM_RAW_FILES; ++file) {
            uint8_t *raw = NULL, *fixed = NULL;
            NtfsAttributeVec attrs = {0};
            valid = ntfs_read_record(&volume, &layout.mft_runs, 24U + file, &raw, &fixed, &error) ==
                        0 &&
                    (infiltratr_load_le16(fixed + 22U) & 3U) == 1U &&
                    ntfs_parse_attributes(fixed, volume.record_size, &attrs, &error) == 0;
            unsigned streams = 0U;
            for (size_t n = 0U; valid && n < attrs.count; ++n) {
                const NtfsAttribute *a = &attrs.items[n];
                if (a->type != NTFS_ATTR_DATA)
                    continue;
                if (++streams != 1U || a->name[0] != '\0' || a->flags != 0U) {
                    valid = false;
                    break;
                }
                const uint64_t bytes =
                    a->nonresident ? a->data_size : infiltratr_load_le32(fixed + a->offset + 16U);
                valid = ldtm_raw_check_file(&check, file, bytes) == 0;
                uint64_t logical = 0U;
                if (!a->nonresident) {
                    valid = valid && bytes == 0U;
                    continue;
                }
                for (size_t run = 0U; valid && logical < bytes && run < a->runs.count; ++run) {
                    uint64_t take = a->runs.items[run].length * NT_UNIT;
                    if (take > bytes - logical)
                        take = bytes - logical;
                    valid = !a->runs.items[run].sparse &&
                            ldtm_raw_check_data(&check, fd, file, a->runs.items[run].lcn * NT_UNIT,
                                                logical, take) == 0;
                    logical += take;
                }
            }
            valid = valid && streams == 1U;
            ntfs_attributes_free(&attrs);
            free(raw);
            free(fixed);
        }
        result = valid ? ldtm_raw_check_finish(&check) : -1;
        ntfs_layout_free(&layout);
        ntfs_close_volume(&volume);
        free(error);
    }
done:
    if (close(fd) != 0)
        result = -1;
    return result;
}
