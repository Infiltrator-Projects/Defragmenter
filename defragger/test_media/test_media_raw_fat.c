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
static int check_chain(int fd, const uint8_t *fat, size_t fat_bytes, uint64_t data_offset, uint32_t unit,
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
        if (ldtm_raw_check_data(check, fd, file, data_offset + (uint64_t)(cluster - 2U) * unit, logical, take) != 0)
            return -1;
        logical += take;
        cluster = fat_get(fat, cluster, bits);
    }
    const uint32_t minimum = bits == 12U ? 0xff8U : bits == 16U ? 0xfff8U : UINT32_C(0x0ffffff8);
    return cluster >= minimum ? 0 : -1;
}

int ldtm_raw_fat(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    /* The writer is exclusively the installed SO. Everything below is a
     * read-only oracle which follows the resulting on-disk allocation. */
    if (create) return ldtm_fat_module_create(spec, path);
    uint64_t capacity = 0U;
    int fd = ldtm_raw_open(path, false, &capacity);
    if (fd < 0)
        return -1;
    const unsigned bits = spec->creator == LDTM_CREATOR_FAT12   ? 12U
                          : spec->creator == LDTM_CREATOR_FAT16 ? 16U
                                                                : 32U;
    uint8_t boot[512] = {0};
    uint32_t spc, reserved, root_entries, root_sectors, spf, data, clusters;
    const uint64_t total = capacity / 512U;
    int result = -1;
    uint8_t *fat = NULL, *root = NULL;
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
    if (data >= total || reserved == 0U)
        goto done;
    clusters = (uint32_t)((total - data) / spc);
    fat = malloc((size_t)spf * 512U);
    root = malloc((size_t)(root_sectors != 0U ? root_sectors : spc) * 512U);
    if (fat == NULL || root == NULL ||
        ldtm_raw_read(fd, (uint64_t)reserved * 512U, fat, (size_t)spf * 512U) != 0)
        goto done;
    const uint32_t root_cluster = bits == 32U ? infiltratr_load_le32(boot + 44U) : 0U;
    if (bits == 32U && (root_cluster < 2U || root_cluster - 2U >= clusters))
        goto done;
    const uint64_t root_offset = bits == 32U ? ((uint64_t)data + (uint64_t)(root_cluster - 2U) * spc) * 512U
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
        if (check_chain(fd, fat, (size_t)spf * 512U, (uint64_t)data * 512U, spc * 512U, clusters, first, bits,
                        (unsigned)file, &check) != 0)
            goto done;
    }
    result = ldtm_raw_check_finish(&check);
done:
    free(fat);
    free(root);
    if (close(fd) != 0)
        result = -1;
    return result;
}
