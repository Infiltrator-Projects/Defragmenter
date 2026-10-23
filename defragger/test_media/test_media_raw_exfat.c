// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t sum32(const uint8_t *data, size_t bytes, bool boot)
{
    uint32_t sum = 0U;
    for (size_t i = 0U; i < bytes; ++i) {
        if (boot && (i == 106U || i == 107U || i == 112U))
            continue;
        sum = (sum >> 1U) | (sum << 31U);
        sum += data[i];
    }
    return sum;
}
static uint16_t sum16(const uint8_t *data, size_t bytes, bool entry)
{
    uint16_t sum = 0U;
    for (size_t i = 0U; i < bytes; ++i) {
        if (entry && (i == 2U || i == 3U))
            continue;
        sum = (uint16_t)((sum >> 1U) | (sum << 15U));
        sum = (uint16_t)(sum + data[i]);
    }
    return sum;
}
static void chain(uint8_t *fat, uint32_t first, uint64_t count, uint32_t next)
{
    for (uint64_t n = 0U; n < count; ++n)
        infiltratr_store_le32(fat + ((uint64_t)first + n) * 4U,
                              n + 1U < count ? first + (uint32_t)n + 1U : next);
}
int ldtm_raw_exfat(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    uint8_t boot[6144] = {0};
    uint32_t spc = 128U, unit = 65536U, fat_offset = 128U;
    uint32_t fat_sectors = (uint32_t)(((capacity / unit + 2U) * 4U + 511U) / 512U);
    fat_sectors = (fat_sectors + spc - 1U) / spc * spc;
    uint32_t heap_sector = fat_offset + fat_sectors;
    uint32_t clusters = (uint32_t)((capacity / 512U - heap_sector) / spc);
    uint32_t root_cluster = 5U;
    uint8_t *fat = NULL, *root = NULL, *bitmap = NULL, *upcase = NULL;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB)
            goto done;
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, unit, (uint64_t)heap_sector / spc + 32U, &plan) != 0)
            goto done;
        const uint64_t heap = (uint64_t)heap_sector / spc;
        fat = calloc(fat_sectors, 512U);
        root = calloc(unit, 1U);
        bitmap = calloc(unit, 1U);
        upcase = calloc(131072U, 1U);
        if (fat == NULL || root == NULL || bitmap == NULL || upcase == NULL)
            goto done;
        infiltratr_store_le32(fat, UINT32_C(0xfffffff8));
        infiltratr_store_le32(fat + 4U, UINT32_MAX);
        chain(fat, 2U, 1U, UINT32_MAX);
        chain(fat, 3U, 2U, UINT32_MAX);
        chain(fat, root_cluster, 1U, UINT32_MAX);
        for (uint32_t c = 2U; c <= 5U; ++c)
            bitmap[(c - 2U) / 8U] |= (uint8_t)(1U << ((c - 2U) % 8U));
        uint64_t allocated = 4U;
        for (uint32_t c = 0U; c < 65536U; ++c)
            infiltratr_store_le16(upcase + c * 2U, (uint16_t)(c >= 97U && c <= 122U ? c - 32U : c));
        root[0U] = 0x83U;
        root[1U] = (uint8_t)strlen(spec->label);
        for (size_t n = 0U; n < strlen(spec->label); ++n)
            infiltratr_store_le16(root + 2U + n * 2U, (uint16_t)(unsigned char)spec->label[n]);
        root[32U] = 0x81U;
        infiltratr_store_le32(root + 52U, 2U);
        infiltratr_store_le64(root + 56U, (clusters + 7U) / 8U);
        root[64U] = 0x82U;
        infiltratr_store_le32(root + 68U, sum32(upcase, 131072U, false));
        infiltratr_store_le32(root + 84U, 3U);
        infiltratr_store_le64(root + 88U, 131072U);
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            const LdtmRawFile *item = &plan.files[file];
            uint8_t *entry = root + 96U + file * 96U;
            char name[16];
            ldtm_raw_name(file, name);
            entry[0U] = 0x85U;
            entry[1U] = 2U;
            infiltratr_store_le16(entry + 4U, 0x20U);
            entry[32U] = 0xc0U;
            entry[33U] = 1U;
            entry[35U] = (uint8_t)strlen(name);
            uint8_t encoded[30] = {0};
            for (size_t n = 0U; n < strlen(name); ++n)
                infiltratr_store_le16(encoded + n * 2U, (uint16_t)(unsigned char)name[n]);
            infiltratr_store_le16(entry + 36U, sum16(encoded, strlen(name) * 2U, false));
            infiltratr_store_le64(entry + 40U, item->bytes);
            const uint32_t first =
                item->count[0] != 0U ? (uint32_t)(item->start[0] - heap + 2U) : 0U;
            infiltratr_store_le32(entry + 52U, first);
            infiltratr_store_le64(entry + 56U, item->bytes);
            entry[64U] = 0xc1U;
            memcpy(entry + 66U, encoded, sizeof(encoded));
            infiltratr_store_le16(entry + 2U, sum16(entry, 96U, true));
            for (unsigned part = 0U; part < 2U; ++part) {
                if (item->count[part] == 0U)
                    continue;
                const uint32_t start = (uint32_t)(item->start[part] - heap + 2U);
                const uint32_t next = part == 0U && item->count[1] != 0U
                                          ? (uint32_t)(item->start[1] - heap + 2U)
                                          : UINT32_MAX;
                chain(fat, start, item->count[part], next);
                for (uint64_t n = 0U; n < item->count[part]; ++n) {
                    const uint64_t bit = (uint64_t)start + n - 2U;
                    bitmap[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
                    ++allocated;
                }
            }
        }
        memcpy(boot,
               "\xeb\x76\x90"
               "EXFAT   ",
               11U);
        infiltratr_store_le64(boot + 72U, capacity / 512U);
        infiltratr_store_le32(boot + 80U, fat_offset);
        infiltratr_store_le32(boot + 84U, fat_sectors);
        infiltratr_store_le32(boot + 88U, heap_sector);
        infiltratr_store_le32(boot + 92U, clusters);
        infiltratr_store_le32(boot + 96U, root_cluster);
        infiltratr_store_le32(boot + 100U, UINT32_C(0x4c44544d));
        infiltratr_store_le16(boot + 104U, 0x100U);
        boot[108U] = 9U;
        boot[109U] = 7U;
        boot[110U] = 1U;
        boot[111U] = 0x80U;
        boot[112U] = (uint8_t)(allocated * 100U / clusters);
        for (unsigned sector = 0U; sector < 11U; ++sector) {
            boot[sector * 512U + 510U] = 0x55U;
            boot[sector * 512U + 511U] = 0xaaU;
        }
        const uint32_t checksum = sum32(boot, 5632U, true);
        for (unsigned offset = 5632U; offset < 6144U; offset += 4U)
            infiltratr_store_le32(boot + offset, checksum);
        if (ldtm_raw_write(fd, 0U, boot, sizeof(boot)) != 0 ||
            ldtm_raw_write(fd, sizeof(boot), boot, sizeof(boot)) != 0 ||
            ldtm_raw_write(fd, (uint64_t)fat_offset * 512U, fat, (size_t)fat_sectors * 512U) != 0 ||
            ldtm_raw_write(fd, heap * unit, bitmap, unit) != 0 ||
            ldtm_raw_write(fd, (heap + 1U) * unit, upcase, 131072U) != 0 ||
            ldtm_raw_write(fd, (heap + 3U) * unit, root, unit) != 0 ||
            ldtm_raw_write_payload(fd, &plan) != 0)
            goto done;
        result = 0;
    } else {
        if (ldtm_raw_read(fd, 0U, boot, sizeof(boot)) != 0 ||
            memcmp(boot + 3U, "EXFAT   ", 8U) != 0 || boot[108U] != 9U || boot[109U] > 7U)
            goto done;
        spc = UINT32_C(1) << boot[109U];
        unit = spc * 512U;
        fat_offset = infiltratr_load_le32(boot + 80U);
        fat_sectors = infiltratr_load_le32(boot + 84U);
        heap_sector = infiltratr_load_le32(boot + 88U);
        clusters = infiltratr_load_le32(boot + 92U);
        root_cluster = infiltratr_load_le32(boot + 96U);
        if (fat_sectors == 0U || fat_sectors > 16384U || heap_sector % spc != 0U ||
            root_cluster < 2U || root_cluster - 2U >= clusters)
            goto done;
        fat = malloc((size_t)fat_sectors * 512U);
        root = malloc(unit);
        if (fat == NULL || root == NULL ||
            ldtm_raw_read(fd, (uint64_t)fat_offset * 512U, fat, (size_t)fat_sectors * 512U) != 0 ||
            ldtm_raw_read(fd, (uint64_t)heap_sector * 512U + (uint64_t)(root_cluster - 2U) * unit,
                          root, unit) != 0)
            goto done;
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        for (size_t offset = 0U; offset + 96U <= unit && root[offset] != 0U;) {
            const uint8_t *entry = root + offset;
            if (entry[0U] != 0x85U) {
                offset += 32U;
                continue;
            }
            if (entry[1U] != 2U || entry[32U] != 0xc0U || entry[64U] != 0xc1U || entry[35U] > 12U ||
                infiltratr_load_le16(entry + 2U) != sum16(entry, 96U, true))
                goto done;
            char name[16] = {0};
            for (unsigned n = 0U; n < entry[35U]; ++n) {
                const uint16_t c = infiltratr_load_le16(entry + 66U + n * 2U);
                if (c > 127U)
                    goto done;
                name[n] = (char)c;
            }
            const int file = ldtm_raw_name_index(name);
            const uint64_t bytes = infiltratr_load_le64(entry + 56U);
            if (file < 0 || ldtm_raw_check_file(&check, (unsigned)file, bytes) != 0)
                goto done;
            uint32_t cluster = infiltratr_load_le32(entry + 52U);
            uint64_t logical = 0U;
            while (logical < bytes) {
                if (cluster < 2U || cluster - 2U >= clusters ||
                    (uint64_t)cluster * 4U + 4U > (uint64_t)fat_sectors * 512U)
                    goto done;
                const uint64_t take = bytes - logical < unit ? bytes - logical : unit;
                if (ldtm_raw_check_data(&check, fd, (unsigned)file,
                                        (uint64_t)heap_sector * 512U +
                                            (uint64_t)(cluster - 2U) * unit,
                                        logical, take) != 0)
                    goto done;
                logical += take;
                cluster = (entry[33U] & 2U) != 0U
                              ? cluster + 1U
                              : infiltratr_load_le32(fat + (uint64_t)cluster * 4U);
            }
            if ((entry[33U] & 2U) == 0U && bytes != 0U && cluster < UINT32_C(0xfffffff8))
                goto done;
            offset += 96U;
        }
        result = ldtm_raw_check_finish(&check);
    }
done:
    free(fat);
    free(root);
    free(bitmap);
    free(upcase);
    if (close(fd) != 0)
        result = -1;
    return result;
}
