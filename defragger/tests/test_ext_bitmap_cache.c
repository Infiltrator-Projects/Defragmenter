// SPDX-License-Identifier: GPL-3.0-or-later
/* Exercise changing descriptor/bitmap pairs and failed cache fills directly.
 * The test owns a temporary byte fixture; no physical device is opened. */
#include "ld_io.h"
static ssize_t changing_descriptor_read(int fd, void *buffer, size_t length,
                                       uint64_t offset);
#define ld_pread_full changing_descriptor_read
#include "../gui/filesystems/ext4/native/ext_disk.c"
#undef ld_pread_full
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "line %d: %s\n", __LINE__, #v); exit(1); } } while (0)

static const uint8_t *next_descriptor;
static unsigned descriptor_reads;
static ssize_t changing_descriptor_read(int fd, void *buffer, size_t length,
                                       uint64_t offset)
{
    ssize_t result = ld_pread_full(fd, buffer, length, offset);
    if (offset == 1024U && length == 1024U) {
        ++descriptor_reads;
        if (next_descriptor != NULL) {
            CHECK(pwrite(fd, next_descriptor, 64U, 1024) == 64);
            next_descriptor = NULL;
        }
    }
    return result;
}

int main(void) {
    char path[] = "/tmp/defragger-ext-bitmap-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); CHECK(unlink(path) == 0);
    CHECK(ftruncate(fd, 8 * 1024) == 0);
    ExtFs fs = {0};
    fs.fd = fd; fs.block_size = 1024U; fs.blocks_count = 8U;
    fs.blocks_per_group = 8U; fs.inodes_per_group = 8U;
    fs.group_count = 1U; fs.desc_size = 64U; fs.desc_per_block = 16U;
    fs.ro_compat = EXT_FEATURE_RO_COMPAT_METADATA_CSUM; fs.csum_seed = 123U;
    uint8_t desc[64] = {0}, block[1024] = {0}, inode[1024] = {0};
    fs.group_descs = desc;
    fs.block_bitmap = calloc(1U, 1024U); fs.inode_bitmap = calloc(1U, 1024U);
    CHECK(fs.block_bitmap && fs.inode_bitmap);
    put_le32(desc, 3U); put_le32(desc + 4U, 4U);
    set_block_bitmap_checksum(&fs, 0U, block);
    put_le16(desc + 26U, (uint16_t)bitmap_checksum(&fs, inode, 1U));
    put_le16(desc + 58U, (uint16_t)(bitmap_checksum(&fs, inode, 1U) >> 16U));
    set_group_checksum(&fs, 0U);
    uint8_t old[64]; memcpy(old, desc, sizeof(old));
    block[0] = 0x7fU; inode[0] = 0x03U;
    set_block_bitmap_checksum(&fs, 0U, block);
    put_le16(desc + 26U, (uint16_t)bitmap_checksum(&fs, inode, 1U));
    put_le16(desc + 58U, (uint16_t)(bitmap_checksum(&fs, inode, 1U) >> 16U));
    set_group_checksum(&fs, 0U);
    CHECK(pwrite(fd, desc, sizeof(desc), 1024) == (ssize_t)sizeof(desc));
    CHECK(pwrite(fd, block, sizeof(block), 3072) == (ssize_t)sizeof(block));
    CHECK(pwrite(fd, inode, sizeof(inode), 4096) == (ssize_t)sizeof(inode));
    char *error = NULL;
    memcpy(desc, old, sizeof(old));
    CHECK(load_block_bitmap(&fs, 0U, &error) == 0 && error == NULL);
    CHECK(fs.block_bitmap_valid && fs.block_bitmap[0] == 0x7fU);
    memcpy(desc, old, sizeof(old));
    CHECK(load_inode_bitmap(&fs, 0U, &error) == 0 && error == NULL);
    CHECK(fs.inode_bitmap_valid && fs.inode_bitmap[0] == 0x03U);

    /* A live descriptor update can fail its checksum on the first refresh.
     * Only the next fully verified descriptor/bitmap pair may fill the cache. */
    uint8_t current[64], changing[64];
    memcpy(current, desc, sizeof(current));
    memcpy(changing, current, sizeof(changing)); changing[30] ^= 1U;
    for (unsigned kind = 0U; kind < 2U; ++kind) {
        CHECK(pwrite(fd, changing, sizeof(changing), 1024) == (ssize_t)sizeof(changing));
        memcpy(desc, old, sizeof(old));
        fs.block_bitmap_valid = false; fs.inode_bitmap_valid = false;
        next_descriptor = current; descriptor_reads = 0U;
        CHECK((kind == 0U ? load_block_bitmap(&fs, 0U, &error) :
                           load_inode_bitmap(&fs, 0U, &error)) == 0);
        CHECK(error == NULL && descriptor_reads == 2U);
        CHECK(memcmp(desc, current, sizeof(current)) == 0);
    }

    /* Writable readers never refresh the locked descriptor snapshot. */
    fs.writable = true; fs.block_bitmap_valid = false;
    memcpy(desc, old, sizeof(old));
    CHECK(load_block_bitmap(&fs, 0U, &error) != 0 && !fs.block_bitmap_valid);
    CHECK(memcmp(desc, old, sizeof(old)) == 0); free(error); error = NULL;
    fs.writable = false;
    /* Persistently damaged bytes remain rejected on every call; a failed fill
     * must never turn the next same-group request into a successful cache hit. */
    block[0] ^= 0x80U; inode[0] ^= 0x80U;
    CHECK(pwrite(fd, block, sizeof(block), 3072) == (ssize_t)sizeof(block));
    CHECK(pwrite(fd, inode, sizeof(inode), 4096) == (ssize_t)sizeof(inode));
    fs.inode_bitmap_valid = false;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        CHECK(load_block_bitmap(&fs, 0U, &error) != 0 && !fs.block_bitmap_valid);
        CHECK(error != NULL); free(error); error = NULL;
        CHECK(load_inode_bitmap(&fs, 0U, &error) != 0 && !fs.inode_bitmap_valid);
        CHECK(error != NULL); free(error); error = NULL;
    }
    /* A malformed refreshed descriptor cannot authorize a new bitmap. */
    memcpy(changing, desc, sizeof(changing)); changing[30] ^= 1U;
    CHECK(pwrite(fd, changing, sizeof(changing), 1024) == (ssize_t)sizeof(changing));
    descriptor_reads = 0U;
    CHECK(load_block_bitmap(&fs, 0U, &error) != 0 && !fs.block_bitmap_valid);
    CHECK(descriptor_reads == 3U);
    CHECK(memcmp(desc, current, sizeof(current)) == 0);
    CHECK(strstr(error, "descriptor checksum") != NULL); free(error);
    free(fs.block_bitmap); free(fs.inode_bitmap); CHECK(close(fd) == 0);
    puts("EXT refreshed checksum pairs, write snapshot and rejected cache fills verified");
    return 0;
}
