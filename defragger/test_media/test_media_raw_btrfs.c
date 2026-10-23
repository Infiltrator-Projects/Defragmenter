// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A single-device mixed-group fixture. 16 KiB leaves hold the complete
 * corpus without expanding the writer's qualified level-zero tree subset. */
#define BT_UNIT 16384U
#define BT_SYSTEM LDTM_MIB
#define BT_MIXED (128U * LDTM_MIB)
#define BT_RELOC (UINT64_MAX - 8U)
typedef struct {
    uint64_t object, offset;
    uint8_t type;
    uint8_t data[256];
    size_t bytes;
} BtItem;
static const uint8_t bt_fsid[16] = "LDTM-BTRFS-FSID";
static const uint8_t bt_chunk[16] = "LDTM-BTRFS-CHNK";
static const uint8_t bt_device[16] = "LDTM-BTRFS-DEV1";
static BtItem *item(BtItem *items, unsigned *count, uint64_t object, uint8_t type, uint64_t offset,
                    size_t bytes)
{
    BtItem *r = &items[(*count)++];
    *r = (BtItem){.object = object, .offset = offset, .type = type, .bytes = bytes};
    return r;
}
static int order(const void *a, const void *b)
{
    const BtItem *x = a, *y = b;
    if (x->object != y->object)
        return x->object < y->object ? -1 : 1;
    if (x->type != y->type)
        return x->type < y->type ? -1 : 1;
    return x->offset < y->offset ? -1 : x->offset != y->offset;
}
static void checksum(uint8_t *raw, size_t bytes)
{
    memset(raw, 0, 32U);
    infiltratr_store_le32(raw, ldtm_raw_crc32c(raw + 32U, bytes - 32U));
}
static int leaf_write(int fd, uint64_t address, uint64_t owner, BtItem *items, unsigned count)
{
    uint8_t raw[BT_UNIT] = {0};
    memcpy(raw + 32U, bt_fsid, 16U);
    infiltratr_store_le64(raw + 48U, address);
    memcpy(raw + 64U, bt_chunk, 16U);
    infiltratr_store_le64(raw + 80U, 1U);
    infiltratr_store_le64(raw + 88U, owner);
    infiltratr_store_le32(raw + 96U, count);
    qsort(items, count, sizeof(*items), order);
    size_t cursor = BT_UNIT;
    for (unsigned n = 0U; n < count; ++n) {
        const BtItem *r = &items[n];
        if (r->bytes > cursor || cursor - r->bytes < 101U + 25U * count)
            return -1;
        cursor -= r->bytes;
        memcpy(raw + cursor, r->data, r->bytes);
        uint8_t *slot = raw + 101U + n * 25U;
        infiltratr_store_le64(slot, r->object);
        slot[8U] = r->type;
        infiltratr_store_le64(slot + 9U, r->offset);
        infiltratr_store_le32(slot + 17U, (uint32_t)(cursor - 101U));
        infiltratr_store_le32(slot + 21U, (uint32_t)r->bytes);
    }
    checksum(raw, sizeof(raw));
    return ldtm_raw_write(fd, address, raw, sizeof(raw));
}
static void chunk(uint8_t data[80], uint64_t logical, uint64_t length, uint64_t flags)
{
    memset(data, 0, 80U);
    infiltratr_store_le64(data, length);
    infiltratr_store_le64(data + 8U, 3U);
    infiltratr_store_le64(data + 16U, 65536U);
    infiltratr_store_le64(data + 24U, flags);
    infiltratr_store_le32(data + 32U, BT_UNIT);
    infiltratr_store_le32(data + 36U, BT_UNIT);
    infiltratr_store_le32(data + 40U, BT_UNIT);
    infiltratr_store_le16(data + 44U, 1U);
    infiltratr_store_le64(data + 48U, 1U);
    infiltratr_store_le64(data + 56U, logical);
    memcpy(data + 64U, bt_device, 16U);
}
static uint32_t name_hash(const char *name)
{
    uint32_t crc = UINT32_MAX - 1U;
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; ++p) {
        crc ^= *p;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (UINT32_C(0x82f63b78) & (0U - (crc & 1U)));
    }
    return crc;
}
static int leaf_read(int fd, uint64_t address, uint64_t capacity, uint8_t raw[BT_UNIT])
{
    if (address > capacity || BT_UNIT > capacity - address ||
        ldtm_raw_read(fd, address, raw, BT_UNIT) != 0 || raw[100U] != 0U ||
        memcmp(raw + 32U, bt_fsid, 16U) != 0 || infiltratr_load_le64(raw + 48U) != address ||
        infiltratr_load_le32(raw) != ldtm_raw_crc32c(raw + 32U, BT_UNIT - 32U) ||
        infiltratr_load_le32(raw + 96U) > (BT_UNIT - 101U) / 25U)
        return -1;
    return 0;
}
static const uint8_t *leaf_data(const uint8_t raw[BT_UNIT], unsigned n, size_t *bytes)
{
    const uint32_t count = infiltratr_load_le32(raw + 96U);
    if (n >= count)
        return NULL;
    const uint8_t *slot = raw + 101U + n * 25U;
    const uint64_t offset = 101U + (uint64_t)infiltratr_load_le32(slot + 17U);
    *bytes = infiltratr_load_le32(slot + 21U);
    return offset >= 101U + 25U * count && offset <= BT_UNIT && *bytes <= BT_UNIT - offset
               ? raw + offset
               : NULL;
}
int ldtm_raw_btrfs(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB)
            goto done;
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, BT_UNIT, 160U * LDTM_MIB / BT_UNIT, &plan) != 0)
            goto done;
        uint64_t payload = 0U;
        for (unsigned f = 0U; f < LDTM_RAW_FILES; ++f)
            payload += (plan.files[f].count[0] + plan.files[f].count[1]) * BT_UNIT;
        const uint64_t used = payload + 7U * BT_UNIT;
        uint8_t super[4096] = {0}, system[80], mixed[80];
        chunk(system, BT_SYSTEM, LDTM_MIB, 2U);
        chunk(mixed, BT_MIXED, capacity - BT_MIXED, 5U);
        memcpy(super + 32U, bt_fsid, 16U);
        infiltratr_store_le64(super + 48U, 65536U);
        memcpy(super + 64U, "_BHRfS_M", 8U);
        infiltratr_store_le64(super + 72U, 1U);
        infiltratr_store_le64(super + 80U, BT_MIXED);
        infiltratr_store_le64(super + 88U, BT_SYSTEM);
        infiltratr_store_le64(super + 112U, capacity);
        infiltratr_store_le64(super + 120U, used);
        infiltratr_store_le64(super + 128U, 256U);
        infiltratr_store_le64(super + 136U, 1U);
        infiltratr_store_le32(super + 144U, BT_UNIT);
        infiltratr_store_le32(super + 148U, BT_UNIT);
        infiltratr_store_le32(super + 152U, BT_UNIT);
        infiltratr_store_le32(super + 156U, BT_UNIT);
        infiltratr_store_le32(super + 160U, 97U);
        infiltratr_store_le64(super + 164U, 1U);
        infiltratr_store_le64(super + 188U, 260U);
        infiltratr_store_le64(super + 201U, 1U);
        infiltratr_store_le64(super + 209U, capacity);
        infiltratr_store_le64(super + 217U, used);
        infiltratr_store_le32(super + 233U, BT_UNIT);
        infiltratr_store_le64(super + 245U, 1U);
        memcpy(super + 267U, bt_device, 16U);
        memcpy(super + 283U, bt_fsid, 16U);
        memcpy(super + 299U, spec->label, strlen(spec->label));
        infiltratr_store_le64(super + 555U, UINT64_MAX);
        infiltratr_store_le64(super + 811U, 256U);
        super[819U] = 228U;
        infiltratr_store_le64(super + 820U, BT_SYSTEM);
        memcpy(super + 828U, system, 80U);
        checksum(super, sizeof(super));
        if (ldtm_raw_write(fd, 65536U, super, sizeof(super)) != 0)
            goto done;
        infiltratr_store_le64(super + 48U, 64U * LDTM_MIB);
        checksum(super, sizeof(super));
        if (ldtm_raw_write(fd, 64U * LDTM_MIB, super, sizeof(super)) != 0)
            goto done;
        BtItem records[112];
        unsigned count = 0U;
        memcpy(item(records, &count, 256U, 228U, BT_SYSTEM, 80U)->data, system, 80U);
        memcpy(item(records, &count, 256U, 228U, BT_MIXED, 80U)->data, mixed, 80U);
        if (leaf_write(fd, BT_SYSTEM, 3U, records, count) != 0)
            goto done;
        count = 0U;
        const uint64_t owners[] = {2U, 4U, 5U, 7U, BT_RELOC};
        const unsigned positions[] = {1U, 3U, 2U, 4U, 5U};
        for (unsigned n = 0U; n < 5U; ++n) {
            BtItem *r = item(records, &count, owners[n], 132U, 1U, 247U);
            infiltratr_store_le64(r->data + 160U, 1U);
            infiltratr_store_le64(r->data + 176U, BT_MIXED + (uint64_t)positions[n] * BT_UNIT);
            infiltratr_store_le32(r->data + 216U, 1U);
            infiltratr_store_le64(r->data + 239U, 1U);
        }
        if (leaf_write(fd, BT_MIXED, 1U, records, count) != 0)
            goto done;
        count = 0U;
        const uint64_t meta_owners[] = {3U, 1U, 2U, 5U, 4U, 7U, BT_RELOC};
        for (unsigned n = 0U; n < 7U; ++n) {
            const uint64_t address = n == 0U ? BT_SYSTEM : BT_MIXED + (uint64_t)(n - 1U) * BT_UNIT;
            BtItem *r = item(records, &count, address, 169U, 0U, 33U);
            infiltratr_store_le64(r->data, 1U);
            infiltratr_store_le64(r->data + 8U, 1U);
            infiltratr_store_le64(r->data + 16U, 2U);
            r->data[24U] = 176U;
            infiltratr_store_le64(r->data + 25U, meta_owners[n]);
        }
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file)
            for (unsigned part = 0U; part < 2U; ++part) {
                const LdtmRawFile *f = &plan.files[file];
                if (f->count[part] == 0U)
                    continue;
                BtItem *r = item(records, &count, f->start[part] * BT_UNIT, 168U,
                                 f->count[part] * BT_UNIT, 53U);
                infiltratr_store_le64(r->data, 1U);
                infiltratr_store_le64(r->data + 8U, 1U);
                infiltratr_store_le64(r->data + 16U, 1U);
                r->data[24U] = 178U;
                infiltratr_store_le64(r->data + 25U, 5U);
                infiltratr_store_le64(r->data + 33U, 257U + file);
                infiltratr_store_le64(r->data + 41U, part == 0U ? 0U : f->count[0] * BT_UNIT);
                infiltratr_store_le32(r->data + 49U, 1U);
            }
        BtItem *group = item(records, &count, BT_SYSTEM, 192U, LDTM_MIB, 24U);
        infiltratr_store_le64(group->data, BT_UNIT);
        infiltratr_store_le64(group->data + 8U, 256U);
        infiltratr_store_le64(group->data + 16U, 2U);
        group = item(records, &count, BT_MIXED, 192U, capacity - BT_MIXED, 24U);
        infiltratr_store_le64(group->data, payload + 6U * BT_UNIT);
        infiltratr_store_le64(group->data + 8U, 256U);
        infiltratr_store_le64(group->data + 16U, 5U);
        if (leaf_write(fd, BT_MIXED + BT_UNIT, 2U, records, count) != 0)
            goto done;
        count = 0U;
        BtItem *root = item(records, &count, 256U, 1U, 0U, 160U);
        infiltratr_store_le32(root->data + 52U, 0040755U);
        infiltratr_store_le32(root->data + 40U, 1U);
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            const LdtmRawFile *f = &plan.files[file];
            char name[16];
            ldtm_raw_name(file, name);
            const size_t length = strlen(name);
            BtItem *r = item(records, &count, 257U + file, 1U, 0U, 160U);
            infiltratr_store_le64(r->data, 1U);
            infiltratr_store_le64(r->data + 8U, 1U);
            infiltratr_store_le64(r->data + 16U, f->bytes);
            infiltratr_store_le64(r->data + 24U, (f->count[0] + f->count[1]) * BT_UNIT);
            infiltratr_store_le32(r->data + 40U, 1U);
            infiltratr_store_le32(r->data + 52U, 0100644U);
            infiltratr_store_le64(r->data + 64U, 1U);
            r = item(records, &count, 257U + file, 12U, 256U, 10U + length);
            infiltratr_store_le64(r->data, 2U + file);
            infiltratr_store_le16(r->data + 8U, (uint16_t)length);
            memcpy(r->data + 10U, name, length);
            for (unsigned dir = 0U; dir < 2U; ++dir) {
                r = item(records, &count, 256U, dir == 0U ? 84U : 96U,
                         dir == 0U ? name_hash(name) : 2U + file, 30U + length);
                infiltratr_store_le64(r->data, 257U + file);
                r->data[8U] = 1U;
                infiltratr_store_le64(r->data + 17U, 1U);
                infiltratr_store_le16(r->data + 27U, (uint16_t)length);
                r->data[29U] = 1U;
                memcpy(r->data + 30U, name, length);
            }
            for (unsigned part = 0U; part < 2U; ++part) {
                if (f->count[part] == 0U)
                    continue;
                r = item(records, &count, 257U + file, 108U,
                         part == 0U ? 0U : f->count[0] * BT_UNIT, 53U);
                infiltratr_store_le64(r->data, 1U);
                infiltratr_store_le64(r->data + 8U, f->count[part] * BT_UNIT);
                r->data[20U] = 1U;
                infiltratr_store_le64(r->data + 21U, f->start[part] * BT_UNIT);
                infiltratr_store_le64(r->data + 29U, f->count[part] * BT_UNIT);
                infiltratr_store_le64(r->data + 45U, f->count[part] * BT_UNIT);
            }
        }
        if (leaf_write(fd, BT_MIXED + 2U * BT_UNIT, 5U, records, count) != 0 ||
            leaf_write(fd, BT_MIXED + 3U * BT_UNIT, 4U, records, 0U) != 0 ||
            leaf_write(fd, BT_MIXED + 4U * BT_UNIT, 7U, records, 0U) != 0 ||
            leaf_write(fd, BT_MIXED + 5U * BT_UNIT, BT_RELOC, records, 0U) != 0)
            goto done;
        result = ldtm_raw_write_payload(fd, &plan);
    } else {
        uint8_t super[4096], raw[BT_UNIT];
        if (ldtm_raw_read(fd, 65536U, super, sizeof(super)) != 0 ||
            memcmp(super + 64U, "_BHRfS_M", 8U) != 0 ||
            infiltratr_load_le32(super) != ldtm_raw_crc32c(super + 32U, sizeof(super) - 32U) ||
            infiltratr_load_le32(super + 148U) != BT_UNIT ||
            leaf_read(fd, infiltratr_load_le64(super + 80U), capacity, raw) != 0)
            goto done;
        uint64_t fs_tree = 0U;
        for (unsigned n = 0U; n < infiltratr_load_le32(raw + 96U); ++n) {
            const uint8_t *slot = raw + 101U + n * 25U;
            size_t bytes = 0U;
            const uint8_t *data = leaf_data(raw, n, &bytes);
            if (data == NULL)
                goto done;
            if (infiltratr_load_le64(slot) == 5U && slot[8U] == 132U && bytes >= 239U)
                fs_tree = infiltratr_load_le64(data + 176U);
        }
        if (fs_tree == 0U || leaf_read(fd, fs_tree, capacity, raw) != 0)
            goto done;
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        for (unsigned n = 0U; n < infiltratr_load_le32(raw + 96U); ++n) {
            const uint8_t *slot = raw + 101U + n * 25U;
            size_t bytes = 0U;
            const uint8_t *data = leaf_data(raw, n, &bytes);
            if (data == NULL)
                goto done;
            const uint64_t id = infiltratr_load_le64(slot);
            if (id == 256U)
                continue;
            if (id < 257U || id >= 257U + LDTM_RAW_FILES)
                goto done;
            const unsigned file = (unsigned)(id - 257U);
            if (slot[8U] == 1U) {
                if (bytes != 160U || infiltratr_load_le32(data + 52U) != 0100644U ||
                    ldtm_raw_check_file(&check, file, infiltratr_load_le64(data + 16U)) != 0)
                    goto done;
            } else if (slot[8U] == 108U) {
                const uint64_t logical = infiltratr_load_le64(slot + 9U),
                               expected = ldtm_raw_file_bytes(spec, file);
                if (bytes != 53U || data[16U] != 0U || data[17U] != 0U || data[20U] != 1U ||
                    logical >= expected)
                    goto done;
                const uint64_t length = infiltratr_load_le64(data + 45U),
                               take = length < expected - logical ? length : expected - logical;
                if (ldtm_raw_check_data(&check, fd, file,
                                        infiltratr_load_le64(data + 21U) +
                                            infiltratr_load_le64(data + 37U),
                                        logical, take) != 0)
                    goto done;
            }
        }
        result = ldtm_raw_check_finish(&check);
    }
done:
    if (close(fd) != 0)
        result = -1;
    return result;
}
