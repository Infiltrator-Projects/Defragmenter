// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "zfs_native.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ZF_START (4U * LDTM_MIB)
#define ZF_UNIT 131072U
#define ZF_LABEL 262144U
typedef struct {
    uint8_t *raw;
    size_t size, at;
    bool valid;
} Xdr;
static void x32(Xdr *w, uint32_t value)
{
    if (!w->valid || w->at > w->size || w->size - w->at < 4U) {
        w->valid = false;
        return;
    }
    infiltratr_store_be32(w->raw + w->at, value);
    w->at += 4U;
}
static void x64(Xdr *w, uint64_t value)
{
    x32(w, (uint32_t)(value >> 32U));
    x32(w, (uint32_t)value);
}
static void xs(Xdr *w, const char *value)
{
    const size_t bytes = strlen(value), padded = (bytes + 3U) & ~(size_t)3U;
    x32(w, (uint32_t)bytes);
    if (!w->valid || w->at > w->size || padded > w->size - w->at) {
        w->valid = false;
        return;
    }
    memcpy(w->raw + w->at, value, bytes);
    w->at += padded;
}
static size_t pair(Xdr *w, const char *name, uint32_t type)
{
    const size_t at = w->at;
    x32(w, 0U);
    x32(w, 0U);
    xs(w, name);
    x32(w, type);
    x32(w, 1U);
    return at;
}
static void pair_end(Xdr *w, size_t at)
{
    if (!w->valid || at > w->at || w->at - at > UINT32_MAX) {
        w->valid = false;
        return;
    }
    infiltratr_store_be32(w->raw + at, (uint32_t)(w->at - at));
    infiltratr_store_be32(w->raw + at + 4U, (uint32_t)(w->at - at));
}
static void uint_pair(Xdr *w, const char *name, uint64_t value)
{
    size_t at = pair(w, name, 8U);
    x64(w, value);
    pair_end(w, at);
}
static void string_pair(Xdr *w, const char *name, const char *value)
{
    size_t at = pair(w, name, 9U);
    xs(w, value);
    pair_end(w, at);
}
static void nv_boundary(Xdr *w)
{
    x32(w, 0U);
    x32(w, 0U);
}
static void fletcher(const uint8_t *raw, size_t bytes, uint64_t sum[4])
{
    memset(sum, 0, 4U * sizeof(*sum));
    for (size_t at = 0U; at < bytes; at += 4U) {
        sum[0] += infiltratr_load_le32(raw + at);
        sum[1] += sum[0];
        sum[2] += sum[1];
        sum[3] += sum[2];
    }
}
static void bp(uint8_t out[128], uint64_t offset, unsigned type, unsigned level, const uint8_t *raw,
               size_t bytes)
{
    memset(out, 0, 128U);
    const uint64_t sectors = bytes / 512U;
    infiltratr_store_le64(out, sectors | (UINT64_C(3) << 32U));
    infiltratr_store_le64(out + 8U, offset / 512U);
    infiltratr_store_le64(out + 48U, (sectors - 1U) | ((sectors - 1U) << 16U) |
                                         (UINT64_C(2) << 32U) | (UINT64_C(7) << 40U) |
                                         ((uint64_t)type << 48U) | ((uint64_t)level << 56U) |
                                         (UINT64_C(1) << 63U));
    infiltratr_store_le64(out + 80U, 10U);
    uint64_t sum[4];
    fletcher(raw, bytes, sum);
    for (unsigned n = 0U; n < 4U; ++n)
        infiltratr_store_le64(out + 96U + n * 8U, sum[n]);
}
static int block_write(int fd, uint64_t offset, unsigned type, unsigned level, const uint8_t *raw,
                       size_t bytes, uint8_t pointer[128])
{
    if (ldtm_raw_write(fd, ZF_START + offset, raw, bytes) != 0)
        return -1;
    bp(pointer, offset, type, level, raw, bytes);
    return 0;
}
static void dnode(uint8_t out[512], unsigned type, unsigned bonus, unsigned sectors,
                  unsigned bonus_bytes, uint64_t max, const uint8_t pointer[128])
{
    memset(out, 0, 512U);
    out[0U] = (uint8_t)type;
    out[2U] = 1U;
    out[3U] = 1U;
    out[4U] = (uint8_t)bonus;
    out[5U] = 7U;
    out[6U] = 2U;
    infiltratr_store_le16(out + 8U, (uint16_t)sectors);
    infiltratr_store_le16(out + 10U, (uint16_t)bonus_bytes);
    infiltratr_store_le64(out + 16U, max);
    infiltratr_store_le64(out + 24U, (uint64_t)sectors * 512U);
    if (pointer != NULL)
        memcpy(out + 64U, pointer, 128U);
}
static void microzap(uint8_t raw[4096])
{
    memset(raw, 0, 4096U);
    infiltratr_store_le64(raw, (UINT64_C(1) << 63U) | 3U);
}
static void zap_entry(uint8_t raw[4096], unsigned n, const char *name, uint64_t value)
{
    uint8_t *entry = raw + 64U + n * 64U;
    infiltratr_store_le64(entry, value);
    memcpy(entry + 14U, name, strlen(name) + 1U);
}
static int bp_read(int fd, uint64_t capacity, const uint8_t pointer[128], unsigned type,
                   unsigned level, uint8_t *raw, size_t bytes, uint64_t *physical)
{
    const uint64_t word = infiltratr_load_le64(pointer), prop = infiltratr_load_le64(pointer + 48U);
    const uint64_t offset = infiltratr_load_le64(pointer + 8U);
    if ((word >> 32U) != 3U || (word & 0xffffffU) != bytes / 512U ||
        offset > (capacity - ZF_START) / 512U || ((prop & 0xffffU) + 1U) * 512U != bytes ||
        (((prop >> 16U) & 0xffffU) + 1U) * 512U != bytes || ((prop >> 32U) & 0x7fU) != 2U ||
        ((prop >> 40U) & 0xffU) != 7U || ((prop >> 48U) & 0xffU) != type ||
        ((prop >> 56U) & 0x7fU) != level || (prop >> 63U) != 1U)
        return -1;
    *physical = ZF_START + offset * 512U;
    if (*physical > capacity || bytes > capacity - *physical ||
        ldtm_raw_read(fd, *physical, raw, bytes) != 0)
        return -1;
    uint64_t sum[4];
    fletcher(raw, bytes, sum);
    for (unsigned n = 0U; n < 4U; ++n)
        if (sum[n] != infiltratr_load_le64(pointer + 96U + n * 8U))
            return -1;
    return 0;
}
int ldtm_raw_zfs(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    uint8_t *data = malloc(ZF_UNIT), *indirect = malloc(65536U);
    if (data == NULL || indirect == NULL || capacity < ZF_START)
        goto done;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB)
            goto done;
        const uint64_t asize = capacity - 8U * LDTM_MIB, slabs = asize / (256U * LDTM_MIB);
        if (slabs != 7U)
            goto done;
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, ZF_UNIT, 36U * LDTM_MIB / ZF_UNIT, &plan) != 0 ||
            plan.end * ZF_UNIT - ZF_START >= 256U * LDTM_MIB)
            goto done;
        if (ldtm_raw_write_payload(fd, &plan) != 0)
            goto done;
        uint8_t files[16384] = {0}, mos_nodes[16384] = {0}, pointer[128], scratch[4096],
                root_pointer[128];
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            const LdtmRawFile *f = &plan.files[file];
            const uint64_t blocks = f->count[0] + f->count[1];
            memset(indirect, 0, 65536U);
            uint64_t logical = 0U;
            for (unsigned part = 0U; part < 2U; ++part)
                for (uint64_t n = 0U; n < f->count[part]; ++n) {
                    const uint64_t physical = (f->start[part] + n) * ZF_UNIT;
                    if (logical >= 512U || ldtm_raw_read(fd, physical, data, ZF_UNIT) != 0)
                        goto done;
                    bp(indirect + logical * 128U, physical - ZF_START, 19U, 0U, data, ZF_UNIT);
                    ++logical;
                }
            uint8_t *dn = files + (file + 1U) * 512U;
            if (blocks != 0U) {
                if (block_write(fd, 8U * LDTM_MIB + (uint64_t)file * 65536U, 19U, 1U, indirect,
                                65536U, pointer) != 0)
                    goto done;
                dnode(dn, 19U, 17U, 256U, 264U, blocks - 1U, pointer);
                dn[1U] = 16U;
                dn[2U] = 2U;
            } else
                dnode(dn, 19U, 17U, 256U, 264U, 0U, NULL);
            infiltratr_store_le64(dn + 192U + 72U, 0100644U);
            infiltratr_store_le64(dn + 192U + 80U, f->bytes);
            infiltratr_store_le64(dn + 192U + 88U, 17U);
            infiltratr_store_le64(dn + 192U + 96U, 1U);
        }
        microzap(scratch);
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            char name[16];
            ldtm_raw_name(file, name);
            zap_entry(scratch, file, name, (UINT64_C(8) << 60U) + file + 1U);
        }
        if (block_write(fd, 10U * LDTM_MIB, 20U, 0U, scratch, sizeof(scratch), pointer) != 0)
            goto done;
        dnode(files + 17U * 512U, 20U, 17U, 8U, 264U, 0U, pointer);
        infiltratr_store_le64(files + 17U * 512U + 264U, 0040755U);
        microzap(scratch);
        zap_entry(scratch, 0U, "ROOT", 17U);
        zap_entry(scratch, 1U, "VERSION", 5U);
        if (block_write(fd, 10U * LDTM_MIB + 4096U, 21U, 0U, scratch, sizeof(scratch), pointer) !=
            0)
            goto done;
        dnode(files + 18U * 512U, 21U, 0U, 8U, 0U, 0U, pointer);
        if (block_write(fd, 7U * LDTM_MIB, 10U, 0U, files, sizeof(files), pointer) != 0)
            goto done;
        memset(scratch, 0, sizeof(scratch));
        dnode(scratch, 10U, 0U, 32U, 0U, 0U, pointer);
        if (block_write(fd, 6U * LDTM_MIB, 11U, 0U, scratch, sizeof(scratch), root_pointer) != 0)
            goto done;
        dnode(mos_nodes + 4U * 512U, 16U, 16U, 8U, 256U, 0U, NULL);
        memcpy(mos_nodes + 4U * 512U + 320U, root_pointer, 128U);
        microzap(scratch);
        zap_entry(scratch, 0U, "features_for_write", 5U);
        if (block_write(fd, LDTM_MIB, 1U, 0U, scratch, sizeof(scratch), pointer) != 0)
            goto done;
        dnode(mos_nodes + 512U, 1U, 0U, 8U, 0U, 0U, pointer);
        microzap(scratch);
        if (block_write(fd, 1536U * 1024U, 1U, 0U, scratch, sizeof(scratch), pointer) != 0)
            goto done;
        dnode(mos_nodes + 5U * 512U, 1U, 0U, 8U, 0U, 0U, pointer);
        uint8_t array[4096] = {0};
        for (unsigned slab = 0U; slab < 7U; ++slab) {
            infiltratr_store_le64(array + slab * 8U, 16U + slab);
            memset(scratch, 0, sizeof(scratch));
            unsigned entries = 0U;
            uint64_t allocated = 0U;
            if (slab == 0U) {
                infiltratr_store_le64(scratch, 4095U);
                entries = 1U;
                allocated = 16U * LDTM_MIB;
                for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file)
                    for (unsigned part = 0U; part < 2U; ++part) {
                        const LdtmRawFile *f = &plan.files[file];
                        if (f->count[part] == 0U)
                            continue;
                        const uint64_t offset = f->start[part] * ZF_UNIT - ZF_START,
                                       count = f->count[part] * ZF_UNIT / 4096U;
                        if (count > 32768U)
                            goto done;
                        infiltratr_store_le64(scratch + entries * 8U,
                                              ((offset / 4096U) << 16U) | (count - 1U));
                        ++entries;
                        allocated += count * 4096U;
                    }
            }
            if (block_write(fd, 5U * LDTM_MIB + (uint64_t)slab * 4096U, 8U, 0U, scratch,
                            sizeof(scratch), pointer) != 0)
                goto done;
            uint8_t *dn = mos_nodes + (16U + slab) * 512U;
            dnode(dn, 8U, 7U, 8U, 24U, 0U, pointer);
            infiltratr_store_le64(dn + 200U, (uint64_t)entries * 8U);
            infiltratr_store_le64(dn + 208U, allocated);
        }
        if (block_write(fd, 4U * LDTM_MIB, 2U, 0U, array, sizeof(array), pointer) != 0)
            goto done;
        dnode(mos_nodes + 2U * 512U, 2U, 0U, 8U, 0U, 0U, pointer);
        if (block_write(fd, 3U * LDTM_MIB, 10U, 0U, mos_nodes, sizeof(mos_nodes), pointer) != 0)
            goto done;
        memset(scratch, 0, sizeof(scratch));
        dnode(scratch, 10U, 0U, 32U, 0U, 0U, pointer);
        if (block_write(fd, 2U * LDTM_MIB, 11U, 0U, scratch, sizeof(scratch), root_pointer) != 0)
            goto done;
        for (unsigned label = 0U; label < 4U; ++label) {
            const uint64_t base = label < 2U ? (uint64_t)label * ZF_LABEL
                                             : capacity - (uint64_t)(4U - label) * ZF_LABEL;
            memset(data, 0, 112U * 1024U);
            data[0U] = 1U;
            Xdr w = {data, 112U * 1024U, 4U, true};
            nv_boundary(&w);
            uint_pair(&w, "version", 28U);
            uint_pair(&w, "state", 1U);
            uint_pair(&w, "txg", 10U);
            string_pair(&w, "name", spec->label);
            uint_pair(&w, "pool_guid", UINT64_C(0x1111222233334444));
            uint_pair(&w, "guid", UINT64_C(0x5555666677778888));
            uint_pair(&w, "top_guid", UINT64_C(0x9999aaaabbbbcccc));
            const size_t tree = pair(&w, "vdev_tree", 19U);
            nv_boundary(&w);
            string_pair(&w, "type", "root");
            const size_t children = pair(&w, "children", 20U);
            nv_boundary(&w);
            string_pair(&w, "type", "disk");
            uint_pair(&w, "id", 3U);
            uint_pair(&w, "guid", UINT64_C(0x5555666677778888));
            uint_pair(&w, "ashift", 12U);
            uint_pair(&w, "metaslab_array", 2U);
            uint_pair(&w, "metaslab_shift", 28U);
            uint_pair(&w, "asize", asize);
            nv_boundary(&w);
            pair_end(&w, children);
            nv_boundary(&w);
            pair_end(&w, tree);
            nv_boundary(&w);
            if (!w.valid || ldtm_raw_write(fd, base + 16384U, data, 112U * 1024U) != 0)
                goto done;
            uint8_t uber[4096] = {0};
            infiltratr_store_le64(uber, UINT64_C(0x00bab10c));
            infiltratr_store_le64(uber + 8U, 28U);
            infiltratr_store_le64(uber + 16U, 10U);
            infiltratr_store_le64(uber + 24U, 77U);
            infiltratr_store_le64(uber + 32U, 1U);
            memcpy(uber + 40U, root_pointer, 128U);
            if (ldtm_raw_write(fd, base + 131072U, uber, sizeof(uber)) != 0)
                goto done;
        }
        result = fsync(fd);
    } else {
        LdZfsSummary summary;
        LdZfsAnalysis analysis = {0};
        char error[256];
        if (zfs_read_summary(path, &summary, error, sizeof(error)) != 0 ||
            zfs_analyse_exact(path, &analysis, error, sizeof(error)) != 0)
            goto done;
        const bool exact = analysis.exact_allocation && analysis.exact_fragmentation &&
                           analysis.files_seen == LDTM_RAW_FILES;
        zfs_analysis_destroy(&analysis);
        if (!exact)
            goto done;
        uint8_t pointer[128], mos[16384], files[16384];
        uint64_t physical = 0U;
        if (ldtm_raw_read(fd, summary.uberblock_magic_offset + 40U, pointer, sizeof(pointer)) !=
                0 ||
            bp_read(fd, capacity, pointer, 11U, 0U, data, 4096U, &physical) != 0)
            goto done;
        memcpy(pointer, data + 64U, 128U);
        if (bp_read(fd, capacity, pointer, 10U, 0U, mos, sizeof(mos), &physical) != 0)
            goto done;
        memcpy(pointer, mos + 4U * 512U + 320U, 128U);
        if (bp_read(fd, capacity, pointer, 11U, 0U, data, 4096U, &physical) != 0)
            goto done;
        memcpy(pointer, data + 64U, 128U);
        if (bp_read(fd, capacity, pointer, 10U, 0U, files, sizeof(files), &physical) != 0)
            goto done;
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            const uint8_t *dn = files + (file + 1U) * 512U;
            const uint64_t bytes = ldtm_raw_file_bytes(spec, file),
                           blocks = (bytes + ZF_UNIT - 1U) / ZF_UNIT;
            if (dn[0U] != 19U || dn[3U] != 1U || dn[4U] != 17U ||
                infiltratr_load_le16(dn + 10U) != 264U ||
                ldtm_raw_check_file(&check, file, infiltratr_load_le64(dn + 272U)) != 0)
                goto done;
            if (blocks == 0U)
                continue;
            if (dn[1U] != 16U || dn[2U] != 2U || infiltratr_load_le64(dn + 16U) != blocks - 1U ||
                bp_read(fd, capacity, dn + 64U, 19U, 1U, indirect, 65536U, &physical) != 0)
                goto done;
            for (uint64_t n = 0U; n < blocks; ++n) {
                if (bp_read(fd, capacity, indirect + n * 128U, 19U, 0U, data, ZF_UNIT, &physical) !=
                    0)
                    goto done;
                const uint64_t logical = n * ZF_UNIT,
                               take = bytes - logical < ZF_UNIT ? bytes - logical : ZF_UNIT;
                if (ldtm_raw_check_data(&check, fd, file, physical, logical, take) != 0)
                    goto done;
            }
        }
        result = ldtm_raw_check_finish(&check);
    }
done:
    free(data);
    free(indirect);
    if (close(fd) != 0)
        result = -1;
    return result;
}
