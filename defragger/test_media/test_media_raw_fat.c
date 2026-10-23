// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t fat_get(const uint8_t *fat, uint32_t cluster, unsigned bits)
{
    if (bits == 12U) {
        const uint16_t pair = infiltratr_load_le16(fat + cluster + cluster / 2U);
        return (cluster & 1U) != 0U ? pair >> 4U : pair & 0xfffU;
    }
    return bits == 16U ? infiltratr_load_le16(fat + cluster * 2U)
                       : infiltratr_load_le32(fat + cluster * 4U) & UINT32_C(0x0fffffff);
}
static void fat_put(uint8_t *fat, uint32_t cluster, uint32_t value, unsigned bits)
{
    if (bits == 12U) {
        uint8_t *p = fat + cluster + cluster / 2U;
        const uint16_t old = infiltratr_load_le16(p);
        const uint32_t pair =
            (cluster & 1U) != 0U ? (old & 15U) | (value << 4U) : (old & 0xf000U) | (value & 0xfffU);
        infiltratr_store_le16(p, (uint16_t)pair);
    } else if (bits == 16U)
        infiltratr_store_le16(fat + cluster * 2U, (uint16_t)value);
    else
        infiltratr_store_le32(fat + cluster * 4U, value);
}
static void fat_chain(uint8_t *fat, const LdtmRawFile *file, uint64_t heap, unsigned bits)
{
    const uint32_t eoc = bits == 12U ? 0xfffU : bits == 16U ? 0xffffU : UINT32_C(0x0fffffff);
    for (unsigned part = 0U; part < 2U; ++part) {
        for (uint64_t n = 0U; n < file->count[part]; ++n) {
            const uint32_t cluster = (uint32_t)(file->start[part] + n - heap + 2U);
            uint32_t next = eoc;
            if (n + 1U < file->count[part])
                next = cluster + 1U;
            else if (part == 0U && file->count[1] != 0U)
                next = (uint32_t)(file->start[1] - heap + 2U);
            fat_put(fat, cluster, next, bits);
        }
    }
}
static int check_chain(int fd, const uint8_t *fat, size_t fat_bytes, uint64_t heap, uint32_t unit,
                       uint32_t clusters, uint32_t first, unsigned bits, unsigned file,
                       LdtmRawCheck *check)
{
    const uint64_t bytes = ldtm_raw_file_bytes(check->spec, file);
    if (ldtm_raw_check_file(check, file, bytes) != 0)
        return -1;
    if (bytes == 0U)
        return first == 0U ? 0 : -1;
    uint32_t cluster = first;
    uint64_t logical = 0U;
    while (logical < bytes) {
        if (cluster < 2U || cluster - 2U >= clusters ||
            (uint64_t)cluster * bits / 8U + (bits == 32U ? 4U : 2U) > fat_bytes)
            return -1;
        const uint64_t take = bytes - logical < unit ? bytes - logical : unit;
        if (ldtm_raw_check_data(check, fd, file, (heap + cluster - 2U) * unit, logical, take) != 0)
            return -1;
        logical += take;
        cluster = fat_get(fat, cluster, bits);
    }
    const uint32_t minimum = bits == 12U ? 0xff8U : bits == 16U ? 0xfff8U : UINT32_C(0x0ffffff8);
    return cluster >= minimum ? 0 : -1;
}

int ldtm_raw_fat(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    const unsigned bits = spec->creator == LDTM_CREATOR_FAT12   ? 12U
                          : spec->creator == LDTM_CREATOR_FAT16 ? 16U
                                                                : 32U;
    uint8_t boot[512] = {0};
    uint32_t spc = bits == 32U ? 8U : 128U;
    uint32_t reserved = bits == 32U ? 32U : spc;
    uint32_t root_entries = bits == 32U ? 0U : spc * 16U;
    uint32_t root_sectors = root_entries / 16U;
    const uint32_t total = (uint32_t)(capacity / 512U);
    uint32_t spf = (uint32_t)((((uint64_t)total / spc + 2U) * bits + 4095U) / 4096U);
    spf = (spf + spc - 1U) / spc * spc;
    uint32_t data = reserved + 2U * spf + root_sectors;
    uint32_t clusters = (total - data) / spc;
    uint64_t heap = data / spc;
    int result = -1;
    uint8_t *fat = NULL, *root = NULL;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB || (bits == 12U && clusters >= 4085U) ||
            (bits == 16U && (clusters < 4085U || clusters >= 65525U)))
            goto done;
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, spc * 512U, heap + 16U, &plan) != 0)
            goto done;
        fat = calloc(spf, 512U);
        root = calloc(root_sectors != 0U ? root_sectors : spc, 512U);
        if (fat == NULL || root == NULL)
            goto done;
        const uint32_t eoc = bits == 12U ? 0xfffU : bits == 16U ? 0xffffU : UINT32_C(0x0fffffff);
        fat_put(fat, 0U, eoc - 7U, bits);
        fat_put(fat, 1U, eoc, bits);
        if (bits == 32U)
            fat_put(fat, 2U, eoc, bits);
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            uint8_t *entry = root + file * 32U;
            char name[16];
            ldtm_raw_name(file, name);
            memset(entry, ' ', 11U);
            memcpy(entry, name, 7U);
            memcpy(entry + 8U, "BIN", 3U);
            entry[11U] = 0x20U;
            const uint32_t first = plan.files[file].count[0] != 0U
                                       ? (uint32_t)(plan.files[file].start[0] - heap + 2U)
                                       : 0U;
            infiltratr_store_le16(entry + 26U, (uint16_t)first);
            if (bits == 32U)
                infiltratr_store_le16(entry + 20U, (uint16_t)(first >> 16U));
            infiltratr_store_le32(entry + 28U, (uint32_t)plan.files[file].bytes);
            fat_chain(fat, &plan.files[file], heap, bits);
        }
        memcpy(boot,
               "\xeb\x3c\x90"
               "MSDOS5.0",
               11U);
        infiltratr_store_le16(boot + 11U, 512U);
        boot[13U] = (uint8_t)spc;
        infiltratr_store_le16(boot + 14U, (uint16_t)reserved);
        boot[16U] = 2U;
        infiltratr_store_le16(boot + 17U, (uint16_t)root_entries);
        boot[21U] = 0xf8U;
        infiltratr_store_le32(boot + 32U, total);
        boot[510U] = 0x55U;
        boot[511U] = 0xaaU;
        infiltratr_store_le16(boot + 24U, 63U);
        infiltratr_store_le16(boot + 26U, 255U);
        const unsigned extended = bits == 32U ? 64U : 36U;
        boot[extended] = 0x80U;
        boot[extended + 2U] = 0x29U;
        infiltratr_store_le32(boot + extended + 3U, UINT32_C(0x4c445400) + bits);
        memset(boot + extended + 7U, ' ', 11U);
        memcpy(boot + extended + 7U, spec->label, strlen(spec->label));
        memcpy(boot + extended + 18U,
               bits == 12U   ? "FAT12   "
               : bits == 16U ? "FAT16   "
                             : "FAT32   ",
               8U);
        if (bits == 32U) {
            infiltratr_store_le32(boot + 36U, spf);
            infiltratr_store_le32(boot + 44U, 2U);
            infiltratr_store_le16(boot + 48U, 1U);
            infiltratr_store_le16(boot + 50U, 6U);
            uint8_t info[512] = {0};
            infiltratr_store_le32(info, UINT32_C(0x41615252));
            infiltratr_store_le32(info + 484U, UINT32_C(0x61417272));
            infiltratr_store_le32(info + 488U, UINT32_MAX);
            infiltratr_store_le32(info + 492U, 3U);
            infiltratr_store_le32(info + 508U, UINT32_C(0xaa550000));
            if (ldtm_raw_write(fd, 512U, info, sizeof(info)) != 0 ||
                ldtm_raw_write(fd, 7U * 512U, info, sizeof(info)) != 0 ||
                ldtm_raw_write(fd, 6U * 512U, boot, sizeof(boot)) != 0)
                goto done;
        } else
            infiltratr_store_le16(boot + 22U, (uint16_t)spf);
        if (ldtm_raw_write(fd, 0U, boot, sizeof(boot)) != 0 ||
            ldtm_raw_write(fd, (uint64_t)reserved * 512U, fat, (size_t)spf * 512U) != 0 ||
            ldtm_raw_write(fd, (uint64_t)(reserved + spf) * 512U, fat, (size_t)spf * 512U) != 0 ||
            ldtm_raw_write(fd, (uint64_t)(bits == 32U ? data : reserved + 2U * spf) * 512U, root,
                           (size_t)(root_sectors != 0U ? root_sectors : spc) * 512U) != 0 ||
            ldtm_raw_write_payload(fd, &plan) != 0)
            goto done;
        result = 0;
    } else {
        if (ldtm_raw_read(fd, 0U, boot, sizeof(boot)) != 0 ||
            infiltratr_load_le16(boot + 11U) != 512U)
            goto done;
        spc = boot[13U];
        reserved = infiltratr_load_le16(boot + 14U);
        root_entries = infiltratr_load_le16(boot + 17U);
        root_sectors = (root_entries + 15U) / 16U;
        spf = bits == 32U ? infiltratr_load_le32(boot + 36U) : infiltratr_load_le16(boot + 22U);
        if (spc == 0U || (spc & (spc - 1U)) != 0U || spf == 0U || spf > 16384U || boot[16U] != 2U)
            goto done;
        data = reserved + 2U * spf + root_sectors;
        if (data % spc != 0U || data >= total)
            goto done;
        heap = data / spc;
        clusters = (total - data) / spc;
        fat = malloc((size_t)spf * 512U);
        root = malloc((size_t)(root_sectors != 0U ? root_sectors : spc) * 512U);
        if (fat == NULL || root == NULL ||
            ldtm_raw_read(fd, (uint64_t)reserved * 512U, fat, (size_t)spf * 512U) != 0)
            goto done;
        const uint32_t root_cluster = bits == 32U ? infiltratr_load_le32(boot + 44U) : 0U;
        if (bits == 32U && (root_cluster < 2U || root_cluster - 2U >= clusters))
            goto done;
        const uint64_t root_offset = bits == 32U ? (heap + root_cluster - 2U) * spc * 512U
                                                 : (uint64_t)(reserved + 2U * spf) * 512U;
        if (ldtm_raw_read(fd, root_offset, root,
                          (size_t)(root_sectors != 0U ? root_sectors : spc) * 512U) != 0)
            goto done;
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        const size_t entries = (size_t)(root_sectors != 0U ? root_sectors : spc) * 16U;
        for (size_t i = 0U; i < entries && root[i * 32U] != 0U; ++i) {
            const uint8_t *entry = root + i * 32U;
            if (entry[0U] == 0xe5U || (entry[11U] & 0x08U) != 0U || entry[11U] == 0x0fU)
                continue;
            if ((entry[11U] & 0x10U) != 0U || memcmp(entry + 8U, "BIN", 3U) != 0)
                goto done;
            char name[16] = {0};
            memcpy(name, entry, 7U);
            memcpy(name + 7U, ".BIN", 5U);
            const int file = ldtm_raw_name_index(name);
            if (file < 0 ||
                infiltratr_load_le32(entry + 28U) != ldtm_raw_file_bytes(spec, (unsigned)file))
                goto done;
            const uint32_t first =
                infiltratr_load_le16(entry + 26U) |
                (bits == 32U ? (uint32_t)infiltratr_load_le16(entry + 20U) << 16U : 0U);
            if (check_chain(fd, fat, (size_t)spf * 512U, heap, spc * 512U, clusters, first, bits,
                            (unsigned)file, &check) != 0)
                goto done;
        }
        result = ldtm_raw_check_finish(&check);
    }
done:
    free(fat);
    free(root);
    if (close(fd) != 0)
        result = -1;
    return result;
}
