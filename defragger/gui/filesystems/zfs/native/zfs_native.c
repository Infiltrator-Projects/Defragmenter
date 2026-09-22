// SPDX-License-Identifier: GPL-3.0-or-later
#include "zfs_native.h"

#include "ld_io.h"
#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * OpenZFS leaf-vdev label geometry.  These constants intentionally describe
 * only the stable on-disk label/uberblock envelope that Defragmenter needs.
 * OpenZFS source is a corroborating reference; production parsing remains
 * first-party and does not link libzfs/libzpool.
 */
#define ZFS_VDEV_PAD_SIZE (8U << 10)
#define ZFS_VDEV_PHYS_SIZE (112U << 10)
#define ZFS_UBERBLOCK_RING_SIZE (128U << 10)
#define ZFS_UBERBLOCK_SIZE (1U << 10)
#define ZFS_VDEV_LABEL_SIZE (ZFS_VDEV_PAD_SIZE * 2U + ZFS_VDEV_PHYS_SIZE + ZFS_UBERBLOCK_RING_SIZE)
#define ZFS_VDEV_LABELS 4U
#define ZFS_UBERBLOCK_RING_OFFSET (ZFS_VDEV_PAD_SIZE * 2U + ZFS_VDEV_PHYS_SIZE)
#define ZFS_UBERBLOCK_SLOTS (ZFS_UBERBLOCK_RING_SIZE / ZFS_UBERBLOCK_SIZE)
#define ZFS_UBERBLOCK_MAGIC UINT64_C(0x00bab10c)
#define ZFS_SPA_VERSION_FEATURES UINT64_C(5000)

static void zfs_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static int size_bytes_for_fd(int fd, const struct stat *status, uint64_t *size_bytes)
{
    if (S_ISREG(status->st_mode)) {
        if (status->st_size < 0) {
            errno = EINVAL;
            return -1;
        }
        *size_bytes = (uint64_t)status->st_size;
        return 0;
    }
    if (S_ISBLK(status->st_mode)) {
        uint64_t bytes = 0U;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;
        *size_bytes = bytes;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static uint64_t load_u64(const uint8_t *data, LdZfsByteOrder byte_order)
{
    return byte_order == LD_ZFS_BYTE_ORDER_LITTLE
        ? infiltratr_load_le64(data)
        : infiltratr_load_be64(data);
}

static bool decode_uberblock(const uint8_t bytes[ZFS_UBERBLOCK_SIZE],
                             LdZfsByteOrder *byte_order,
                             uint64_t *version, uint64_t *txg,
                             uint64_t *guid_sum, uint64_t *timestamp)
{
    const uint64_t little_magic = infiltratr_load_le64(bytes);
    const uint64_t big_magic = infiltratr_load_be64(bytes);
    if (little_magic == ZFS_UBERBLOCK_MAGIC)
        *byte_order = LD_ZFS_BYTE_ORDER_LITTLE;
    else if (big_magic == ZFS_UBERBLOCK_MAGIC)
        *byte_order = LD_ZFS_BYTE_ORDER_BIG;
    else
        return false;

    *version = load_u64(bytes + 8U, *byte_order);
    *txg = load_u64(bytes + 16U, *byte_order);
    *guid_sum = load_u64(bytes + 24U, *byte_order);
    *timestamp = load_u64(bytes + 32U, *byte_order);

    /*
     * A bare magic word is not enough to identify a member.  Real committed
     * uberblocks carry a non-zero transaction group and a recognised SPA
     * version (legacy versions or feature-flags version 5000).
     */
    return *version != 0U && *version <= ZFS_SPA_VERSION_FEATURES &&
           *txg != 0U;
}

static uint64_t label_offset(uint64_t psize, uint32_t label)
{
    const uint64_t ordinal = (uint64_t)label * ZFS_VDEV_LABEL_SIZE;
    return label < (ZFS_VDEV_LABELS / 2U)
        ? ordinal
        : psize - (uint64_t)ZFS_VDEV_LABELS * ZFS_VDEV_LABEL_SIZE + ordinal;
}

static void decode_root_block_pointer(const uint8_t *uberblock,
                                      LdZfsByteOrder byte_order,
                                      LdZfsSummary *summary)
{
    const uint8_t *root = uberblock + 40U;
    const uint64_t dva_word0 = load_u64(root + 0U, byte_order);
    const uint64_t dva_word1 = load_u64(root + 8U, byte_order);
    const uint64_t prop = load_u64(root + 48U, byte_order);

    summary->root_asize = (dva_word0 & UINT64_C(0xffffff)) << 9U;
    summary->root_vdev = (dva_word0 >> 32U) & UINT64_C(0xffffff);
    summary->root_offset =
        (dva_word1 & UINT64_C(0x7fffffffffffffff)) << 9U;
    summary->root_embedded = ((prop >> 39U) & 1U) != 0U;
    if (!summary->root_embedded) {
        summary->root_lsize =
            (((prop >> 0U) & UINT64_C(0xffff)) + 1U) << 9U;
        summary->root_psize =
            (((prop >> 16U) & UINT64_C(0xffff)) + 1U) << 9U;
    }
    summary->root_compression = (uint32_t)((prop >> 32U) & UINT64_C(0x7f));
    summary->root_checksum = (uint32_t)((prop >> 40U) & UINT64_C(0xff));
    summary->root_type = (uint32_t)((prop >> 48U) & UINT64_C(0xff));
    summary->root_level = (uint32_t)((prop >> 56U) & UINT64_C(0x1f));
    summary->root_logical_birth = load_u64(root + 80U, byte_order);
}

static bool better_uberblock(uint64_t txg, uint64_t timestamp,
                             const LdZfsSummary *summary)
{
    if (summary->candidate_uberblocks == 0U)
        return true;
    if (txg != summary->uberblock_txg)
        return txg > summary->uberblock_txg;
    return timestamp > summary->uberblock_timestamp;
}

static int scan_labels(int fd, uint64_t psize, LdZfsSummary *summary)
{
    uint8_t bytes[ZFS_UBERBLOCK_SIZE];

    for (uint32_t label = 0U; label < ZFS_VDEV_LABELS; ++label) {
        const uint64_t base = label_offset(psize, label);
        const uint64_t ring = base + ZFS_UBERBLOCK_RING_OFFSET;
        for (uint32_t slot = 0U; slot < ZFS_UBERBLOCK_SLOTS; ++slot) {
            const uint64_t offset =
                ring + (uint64_t)slot * ZFS_UBERBLOCK_SIZE;
            const ssize_t count =
                ld_pread_full(fd, bytes, sizeof(bytes), offset);
            if (count < 0)
                return -1;
            if ((size_t)count != sizeof(bytes)) {
                errno = EIO;
                return -1;
            }

            LdZfsByteOrder byte_order = LD_ZFS_BYTE_ORDER_LITTLE;
            uint64_t version = 0U;
            uint64_t txg = 0U;
            uint64_t guid_sum = 0U;
            uint64_t timestamp = 0U;
            if (!decode_uberblock(bytes, &byte_order, &version, &txg,
                                  &guid_sum, &timestamp))
                continue;

            if (summary->candidate_uberblocks != UINT32_MAX)
                summary->candidate_uberblocks++;
            if (!better_uberblock(txg, timestamp, summary))
                continue;

            summary->uberblock_magic_offset = offset;
            summary->uberblock_txg = txg;
            summary->uberblock_version = version;
            summary->uberblock_guid_sum = guid_sum;
            summary->uberblock_timestamp = timestamp;
            summary->label_index = label;
            summary->uberblock_slot = slot;
            summary->byte_order = byte_order;
            decode_root_block_pointer(bytes, byte_order, summary);
        }
    }
    return summary->candidate_uberblocks == 0U ? 1 : 0;
}

int zfs_read_summary(const char *path, LdZfsSummary *summary,
                     char *error, size_t error_size)
{
    if (path == NULL || summary == NULL) {
        zfs_error(error, error_size, "invalid ZFS summary request");
        return -1;
    }
    memset(summary, 0, sizeof(*summary));

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }

    struct stat status;
    if (fstat(fd, &status) != 0 ||
        size_bytes_for_fd(fd, &status, &summary->size_bytes) != 0) {
        const int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        return -1;
    }

    /*
     * OpenZFS aligns the physical vdev size to the 256 KiB label size before
     * calculating the two end labels.  Mirror that rule without requiring the
     * backing regular file or block device itself to end on that boundary.
     */
    const uint64_t psize =
        summary->size_bytes - (summary->size_bytes % ZFS_VDEV_LABEL_SIZE);
    int result = 1;
    if (psize >= (uint64_t)ZFS_VDEV_LABELS * ZFS_VDEV_LABEL_SIZE)
        result = scan_labels(fd, psize, summary);

    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;

    if (result < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot read ZFS vdev labels: %s", strerror(errno));
        return -1;
    }
    if (result != 0) {
        zfs_error(error, error_size,
                  "not a recognised ZFS member with a committed label uberblock");
        return -1;
    }
    if (error != NULL && error_size != 0U)
        error[0] = '\0';
    return 0;
}

bool zfs_probe(const char *path)
{
    LdZfsSummary summary;
    return zfs_read_summary(path, &summary, NULL, 0U) == 0;
}

const char *zfs_byte_order_name(const LdZfsSummary *summary)
{
    if (summary == NULL)
        return "unknown";
    return summary->byte_order == LD_ZFS_BYTE_ORDER_LITTLE ? "little" : "big";
}
