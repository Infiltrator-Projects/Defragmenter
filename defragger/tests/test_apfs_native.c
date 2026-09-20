// SPDX-License-Identifier: GPL-3.0-or-later
#include "apfs_native.h"

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_le64(uint8_t *p, uint64_t value)
{
    for (unsigned int index = 0U; index < 8U; ++index)
        p[index] = (uint8_t)(value >> (index * 8U));
}

static int write_image(int fd, const uint8_t *block)
{
    /*
     * apfs_read_summary() now validates that the NX geometry fits the actual
     * target. Keep the fixture sparse, but give it the capacity declared in
     * the synthetic NX superblock.
     */
    const uint64_t block_size = (uint64_t)block[36] |
                                ((uint64_t)block[37] << 8U) |
                                ((uint64_t)block[38] << 16U) |
                                ((uint64_t)block[39] << 24U);
    uint64_t block_count = 0U;
    for (unsigned int index = 0U; index < 8U; ++index)
        block_count |= (uint64_t)block[40U + index] << (index * 8U);
    if (block_size == 0U || block_count > (uint64_t)INT64_MAX / block_size)
        return -1;
    if (ftruncate(fd, (off_t)(block_size * block_count)) != 0) return -1;
    size_t done = 0U;
    while (done < 4096U) {
        const ssize_t count = pwrite(fd, block + done, 4096U - done, (off_t)done);
        if (count <= 0) return -1;
        done += (size_t)count;
    }
    return fsync(fd);
}

static int check(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "APFS native test: %s\n", message);
    return 1;
}

int main(void)
{
    char path[] = "/tmp/linux-defragger-apfs-test.XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }

    uint8_t block[4096] = {0};
    memcpy(block + 32, "NXSB", 4U);
    write_le32(block + 36, 4096U);
    write_le64(block + 40, UINT64_C(123456));
    for (unsigned int index = 0U; index < 16U; ++index)
        block[72U + index] = (uint8_t)(index + 1U);

    if (write_image(fd, block) != 0) {
        perror("write synthetic APFS image");
        (void)close(fd);
        (void)unlink(path);
        return 1;
    }

    ApfsSummary summary;
    char error[256];
    int failed = 0;
    failed |= check(apfs_read_summary(path, &summary, error, sizeof(error)) == 0,
                    "valid NX superblock was rejected");
    failed |= check(summary.block_size == 4096U, "wrong block size");
    failed |= check(summary.block_count == UINT64_C(123456), "wrong block count");
    for (unsigned int index = 0U; index < 16U; ++index)
        failed |= check(summary.container_uuid[index] == (uint8_t)(index + 1U),
                        "wrong container UUID");
    failed |= check(apfs_probe(path), "probe rejected valid APFS image");

    write_le32(block + 36, 2048U);
    if (write_image(fd, block) != 0) failed = 1;
    failed |= check(apfs_read_summary(path, &summary, error, sizeof(error)) != 0,
                    "invalid block size was accepted");
    failed |= check(!apfs_probe(path), "probe accepted invalid APFS geometry");

    write_le32(block + 36, 4096U);
    memcpy(block + 32, "NOPE", 4U);
    if (write_image(fd, block) != 0) failed = 1;
    failed |= check(apfs_read_summary(path, &summary, error, sizeof(error)) != 0,
                    "non-APFS magic was accepted");
    failed |= check(!apfs_probe(path), "probe accepted non-APFS image");

    (void)close(fd);
    (void)unlink(path);
    return failed == 0 ? 0 : 1;
}
