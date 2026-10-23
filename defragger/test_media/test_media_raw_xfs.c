// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "xfs_native.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XF_UNIT 4096U
typedef struct {
    uint32_t start, count;
} FreeRun;
static const uint8_t xf_uuid[16] = "LDTM-XFS-FIXTURE";
static void crc(uint8_t *raw, size_t bytes, size_t field)
{
    memset(raw + field, 0, 4U);
    infiltratr_store_le32(raw + field, ldtm_raw_crc32c(raw, bytes));
}
static void tree(uint8_t raw[XF_UNIT], const char *magic, uint32_t block, unsigned count)
{
    memset(raw, 0, XF_UNIT);
    memcpy(raw, magic, 4U);
    infiltratr_store_be16(raw + 6U, (uint16_t)count);
    infiltratr_store_be32(raw + 8U, UINT32_MAX);
    infiltratr_store_be32(raw + 12U, UINT32_MAX);
    infiltratr_store_be64(raw + 16U, (uint64_t)block * 8U);
    memcpy(raw + 32U, xf_uuid, 16U);
}
static int length_order(const void *a, const void *b)
{
    const FreeRun *left = a, *right = b;
    if (left->count != right->count)
        return left->count < right->count ? -1 : 1;
    return left->start < right->start ? -1 : left->start != right->start;
}
int ldtm_raw_xfs(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB || capacity / XF_UNIT != 524288U)
            goto done;
        const uint32_t blocks = (uint32_t)(capacity / XF_UNIT);
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, XF_UNIT, 2048U, &plan) != 0)
            goto done;
        FreeRun runs[64];
        unsigned run_count = 0U;
        uint32_t free_count = 0U, longest = 0U, begin = 0U;
        bool in_free = false;
        for (uint32_t block = 0U; block <= blocks; ++block) {
            bool used = block == blocks || block < 80U;
            for (unsigned file = 0U; !used && file < LDTM_RAW_FILES; ++file)
                for (unsigned part = 0U; part < 2U; ++part)
                    if (plan.files[file].count[part] != 0U &&
                        block >= plan.files[file].start[part] &&
                        block - plan.files[file].start[part] < plan.files[file].count[part])
                        used = true;
            if (!used) {
                if (!in_free) {
                    begin = block;
                    in_free = true;
                }
                ++free_count;
            } else if (in_free) {
                if (run_count >= 64U)
                    goto done;
                runs[run_count++] = (FreeRun){begin, block - begin};
                if (block - begin > longest)
                    longest = block - begin;
                in_free = false;
            }
        }
        uint8_t ag[XF_UNIT] = {0}, raw[XF_UNIT], inodes[32768] = {0};
        uint8_t *sb = ag, *agf = ag + 512U, *agi = ag + 1024U, *agfl = ag + 1536U;
        memcpy(sb, "XFSB", 4U);
        infiltratr_store_be32(sb + 4U, XF_UNIT);
        infiltratr_store_be64(sb + 8U, blocks);
        memcpy(sb + 32U, xf_uuid, 16U);
        infiltratr_store_be64(sb + 48U, 16U);
        infiltratr_store_be64(sb + 56U, 64U);
        infiltratr_store_be32(sb + 84U, blocks);
        infiltratr_store_be32(sb + 88U, 1U);
        infiltratr_store_be32(sb + 96U, 64U);
        infiltratr_store_be16(sb + 100U, 5U);
        infiltratr_store_be16(sb + 102U, 512U);
        infiltratr_store_be16(sb + 104U, 512U);
        infiltratr_store_be16(sb + 106U, 8U);
        memcpy(sb + 108U, spec->label, strlen(spec->label));
        sb[120U] = 12U;
        sb[121U] = 9U;
        sb[122U] = 9U;
        sb[123U] = 3U;
        sb[124U] = 19U;
        sb[127U] = 25U;
        infiltratr_store_be64(sb + 128U, 64U);
        infiltratr_store_be64(sb + 136U, 47U);
        infiltratr_store_be64(sb + 144U, free_count);
        infiltratr_store_be32(sb + 216U, 1U);
        crc(sb, 512U, 224U);
        memcpy(agf, "XAGF", 4U);
        infiltratr_store_be32(agf + 4U, 1U);
        infiltratr_store_be32(agf + 12U, blocks);
        infiltratr_store_be32(agf + 16U, 1U);
        infiltratr_store_be32(agf + 20U, 2U);
        infiltratr_store_be32(agf + 28U, 1U);
        infiltratr_store_be32(agf + 32U, 1U);
        infiltratr_store_be32(agf + 48U, 4U);
        infiltratr_store_be32(agf + 44U, 3U);
        infiltratr_store_be32(agf + 52U, free_count);
        infiltratr_store_be32(agf + 56U, longest);
        memcpy(agf + 64U, xf_uuid, 16U);
        crc(agf, 512U, 216U);
        memcpy(agi, "XAGI", 4U);
        infiltratr_store_be32(agi + 4U, 1U);
        infiltratr_store_be32(agi + 12U, blocks);
        infiltratr_store_be32(agi + 16U, 64U);
        infiltratr_store_be32(agi + 20U, 3U);
        infiltratr_store_be32(agi + 24U, 1U);
        infiltratr_store_be32(agi + 28U, 47U);
        infiltratr_store_be32(agi + 32U, 64U);
        for (unsigned n = 0U; n < 64U; ++n)
            infiltratr_store_be32(agi + 40U + n * 4U, UINT32_MAX);
        memcpy(agi + 296U, xf_uuid, 16U);
        crc(agi, 512U, 312U);
        memcpy(agfl, "XAFL", 4U);
        memcpy(agfl + 8U, xf_uuid, 16U);
        for (unsigned n = 36U; n < 512U; n += 4U)
            infiltratr_store_be32(agfl + n, UINT32_MAX);
        for (unsigned n = 0U; n < 4U; ++n)
            infiltratr_store_be32(agfl + 36U + n * 4U, 4U + n);
        crc(agfl, 512U, 32U);
        tree(raw, "AB3B", 1U, run_count);
        for (unsigned n = 0U; n < run_count; ++n) {
            infiltratr_store_be32(raw + 56U + n * 8U, runs[n].start);
            infiltratr_store_be32(raw + 60U + n * 8U, runs[n].count);
        }
        crc(raw, sizeof(raw), 52U);
        if (ldtm_raw_write(fd, 1U * XF_UNIT, raw, sizeof(raw)) != 0)
            goto done;
        qsort(runs, run_count, sizeof(runs[0]), length_order);
        tree(raw, "AB3C", 2U, run_count);
        for (unsigned n = 0U; n < run_count; ++n) {
            infiltratr_store_be32(raw + 56U + n * 8U, runs[n].start);
            infiltratr_store_be32(raw + 60U + n * 8U, runs[n].count);
        }
        crc(raw, sizeof(raw), 52U);
        if (ldtm_raw_write(fd, 2U * XF_UNIT, raw, sizeof(raw)) != 0)
            goto done;
        tree(raw, "IAB3", 3U, 1U);
        infiltratr_store_be32(raw + 56U, 64U);
        infiltratr_store_be32(raw + 60U, 47U);
        infiltratr_store_be64(raw + 64U, UINT64_MAX << 17U);
        crc(raw, sizeof(raw), 52U);
        if (ldtm_raw_write(fd, 3U * XF_UNIT, raw, sizeof(raw)) != 0)
            goto done;
        for (unsigned n = 0U; n < 64U; ++n) {
            uint8_t *inode = inodes + n * 512U;
            infiltratr_store_be16(inode, 0x494eU);
            inode[4U] = 3U;
            inode[5U] = 2U;
            infiltratr_store_be64(inode + 152U, 64U + n);
            memcpy(inode + 160U, xf_uuid, 16U);
            if (n == 0U) {
                infiltratr_store_be16(inode + 2U, 0040755U);
                infiltratr_store_be32(inode + 16U, 2U);
                inode[5U] = 1U;
                uint8_t *dir = inode + 176U;
                dir[0U] = LDTM_RAW_FILES;
                infiltratr_store_be32(dir + 2U, 64U);
                size_t position = 6U;
                for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
                    char name[16];
                    ldtm_raw_name(file, name);
                    const size_t length = strlen(name);
                    dir[position] = (uint8_t)length;
                    infiltratr_store_be16(dir + position + 1U, (uint16_t)(0x30U + file * 24U));
                    memcpy(dir + position + 3U, name, length);
                    dir[position + 3U + length] = 1U;
                    infiltratr_store_be32(dir + position + 4U + length, 65U + file);
                    position += length + 8U;
                }
                infiltratr_store_be64(inode + 56U, position);
            } else if (n <= LDTM_RAW_FILES) {
                const LdtmRawFile *item = &plan.files[n - 1U];
                infiltratr_store_be16(inode + 2U, 0100644U);
                infiltratr_store_be32(inode + 16U, 1U);
                infiltratr_store_be64(inode + 56U, item->bytes);
                infiltratr_store_be64(inode + 64U, item->count[0] + item->count[1]);
                unsigned entries = 0U;
                for (unsigned part = 0U; part < 2U; ++part) {
                    if (item->count[part] == 0U)
                        continue;
                    const uint64_t logical = part == 0U ? 0U : item->count[0],
                                   physical = item->start[part];
                    uint8_t *extent = inode + 176U + entries * 16U;
                    infiltratr_store_be64(extent, (logical << 9U) | (physical >> 43U));
                    infiltratr_store_be64(extent + 8U, (physical << 21U) | item->count[part]);
                    ++entries;
                }
                infiltratr_store_be32(inode + 76U, entries);
            }
            crc(inode, 512U, 100U);
        }
        if (ldtm_raw_write(fd, 0U, ag, sizeof(ag)) != 0 ||
            ldtm_raw_write(fd, 8U * XF_UNIT, inodes, sizeof(inodes)) != 0)
            goto done;
        uint8_t log[512] = {0};
        for (unsigned bb = 0U; bb < 10U; ++bb) {
            infiltratr_store_be32(log, 7U);
            if (ldtm_raw_write(fd, 16U * XF_UNIT + (uint64_t)bb * 512U, log, sizeof(log)) != 0)
                goto done;
        }
        memset(log, 0, sizeof(log));
        infiltratr_store_be32(log, UINT32_C(0xfeedbabe));
        infiltratr_store_be32(log + 4U, 7U);
        infiltratr_store_be32(log + 8U, 2U);
        infiltratr_store_be32(log + 12U, 20U);
        infiltratr_store_be64(log + 16U, (UINT64_C(7) << 32U) | 8U);
        infiltratr_store_be64(log + 24U, (UINT64_C(7) << 32U) | 8U);
        infiltratr_store_be32(log + 40U, 1U);
        infiltratr_store_be32(log + 300U, 1U);
        memcpy(log + 304U, xf_uuid, 16U);
        infiltratr_store_be32(log + 320U, 32768U);
        if (ldtm_raw_write(fd, 16U * XF_UNIT + 8U * 512U, log, sizeof(log)) != 0)
            goto done;
        memset(log, 0, sizeof(log));
        infiltratr_store_be32(log, 7U);
        infiltratr_store_be32(log + 4U, 8U);
        log[8U] = 0xaaU;
        log[9U] = 0x20U;
        if (ldtm_raw_write(fd, 16U * XF_UNIT + 9U * 512U, log, sizeof(log)) != 0)
            goto done;
        result = ldtm_raw_write_payload(fd, &plan);
    } else {
        XfsCatalogue catalog = {0};
        char *error = NULL;
        if (xfs_scan_catalogue(path, true, &catalog, &error) != 0) {
            free(error);
            goto done;
        }
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        bool valid =
            catalog.geometry.block_size == XF_UNIT && catalog.regular_files == LDTM_RAW_FILES;
        for (size_t n = 0U; valid && n < catalog.objects.count; ++n) {
            const XfsObject *item = &catalog.objects.items[n];
            if (!item->is_file)
                continue;
            if (item->inode < 65U || item->inode >= 65U + LDTM_RAW_FILES) {
                valid = false;
                break;
            }
            const unsigned file = (unsigned)(item->inode - 65U);
            valid = ldtm_raw_check_file(&check, file, item->size) == 0;
            for (size_t extent = 0U; valid && extent < item->extents.count; ++extent) {
                const XfsExtent *e = &item->extents.items[extent];
                const uint64_t logical = e->logical * XF_UNIT;
                if (logical >= item->size) {
                    valid = false;
                    break;
                }
                const uint64_t take = e->length * XF_UNIT < item->size - logical
                                          ? e->length * XF_UNIT
                                          : item->size - logical;
                valid =
                    !e->unwritten && ldtm_raw_check_data(&check, fd, file, e->physical * XF_UNIT,
                                                         logical, take) == 0;
            }
        }
        result = valid ? ldtm_raw_check_finish(&check) : -1;
        xfs_catalogue_free(&catalog);
        free(error);
    }
done:
    if (close(fd) != 0)
        result = -1;
    return result;
}
