// SPDX-License-Identifier: GPL-3.0-or-later
/* Temporary FAT32-only fixture producer. FAT12/16 use their installed SOs;
 * this legacy producer cannot format either of those completed modules. */
#include "test_media_raw.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fat_chain(uint8_t *fat, const LdtmRawFile *file, uint64_t heap)
{
    const uint32_t eoc = UINT32_C(0x0fffffff);
    for (unsigned part = 0U; part < 2U; ++part) {
        for (uint64_t n = 0U; n < file->count[part]; ++n) {
            const uint32_t cluster = (uint32_t)(file->start[part] + n - heap + 2U);
            uint32_t next = eoc;
            if (n + 1U < file->count[part])
                next = cluster + 1U;
            else if (part == 0U && file->count[1] != 0U)
                next = (uint32_t)(file->start[1] - heap + 2U);
            infiltratr_store_le32(fat + cluster * 4U, next);
        }
    }
}
int ldtm_raw_fat32_create(const LdtmFilesystemSpec *spec, const char *path)
{
    if (!spec || spec->creator != LDTM_CREATOR_FAT32) return -1;
    uint64_t capacity = 0U;
    int fd = ldtm_raw_open(path, true, &capacity);
    if (fd < 0) return -1;
    uint8_t boot[512] = {0};
    const uint32_t spc = 8U, reserved = 32U, root_entries = 0U, root_sectors = 0U;
    const uint32_t total = (uint32_t)(capacity / 512U);
    uint32_t spf = (uint32_t)((((uint64_t)total / spc + 2U) * 32U + 4095U) / 4096U);
    spf = (spf + spc - 1U) / spc * spc;
    const uint32_t data = reserved + 2U * spf;
    const uint64_t heap = data / spc;
    int result = -1;
    uint8_t *fat = NULL, *root = NULL;
    if (capacity != (uint64_t)spec->size_mib * LDTM_MIB)
        goto done;
    LdtmRawPlan plan;
    if (ldtm_raw_plan(spec, capacity, spc * 512U, heap + 16U, &plan) != 0)
        goto done;
    fat = calloc(spf, 512U);
    root = calloc(root_sectors != 0U ? root_sectors : spc, 512U);
    if (fat == NULL || root == NULL)
        goto done;
    const uint32_t eoc = UINT32_C(0x0fffffff);
    infiltratr_store_le32(fat, eoc - 7U);
    infiltratr_store_le32(fat + 4U, eoc);
    infiltratr_store_le32(fat + 8U, eoc);
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
        infiltratr_store_le16(entry + 20U, (uint16_t)(first >> 16U));
        infiltratr_store_le32(entry + 28U, (uint32_t)plan.files[file].bytes);
        fat_chain(fat, &plan.files[file], heap);
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
    const unsigned extended = 64U;
    boot[extended] = 0x80U;
    boot[extended + 2U] = 0x29U;
    infiltratr_store_le32(boot + extended + 3U, UINT32_C(0x4c445400) + 32U);
    memset(boot + extended + 7U, ' ', 11U);
    memcpy(boot + extended + 7U, spec->label, strlen(spec->label));
    memcpy(boot + extended + 18U,
           "FAT32   ",
           8U);
    {
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
    }
    if (ldtm_raw_write(fd, 0U, boot, sizeof(boot)) != 0 ||
        ldtm_raw_write(fd, (uint64_t)reserved * 512U, fat, (size_t)spf * 512U) != 0 ||
        ldtm_raw_write(fd, (uint64_t)(reserved + spf) * 512U, fat, (size_t)spf * 512U) != 0 ||
        ldtm_raw_write(fd, (uint64_t)data * 512U, root,
                       (size_t)(root_sectors != 0U ? root_sectors : spc) * 512U) != 0 ||
        ldtm_raw_write_payload(fd, &plan) != 0)
        goto done;
    result = 0;
done:
    free(fat);
    free(root);
    if (close(fd) != 0) result = -1;
    return result;
}
