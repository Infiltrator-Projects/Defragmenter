// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "ld_device.h"
#include "infiltratr/posix_io.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

uint64_t ldtm_raw_file_bytes(const LdtmFilesystemSpec *spec, unsigned file)
{
    static const uint32_t edges[] = {0U, 1U, 511U, 512U, 513U, 4095U, 4096U, 4097U};
    if (file < LDTM_TARGET_FILE_COUNT) {
        const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
        return ldtm_profile_file_bytes(&profile, file);
    }
    return file < LDTM_RAW_FILES ? edges[file - LDTM_TARGET_FILE_COUNT] : 0U;
}

void ldtm_raw_name(unsigned file, char output[16])
{
    (void)snprintf(output, 16U, "F%06u.BIN", file);
}

int ldtm_raw_name_index(const char *name)
{
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
        char expected[16];
        ldtm_raw_name(file, expected);
        if (strcmp(name, expected) == 0)
            return (int)file;
    }
    return -1;
}

int ldtm_raw_open(const char *path, bool write, uint64_t *capacity)
{
    const int fd = ld_device_open_verified_fd(path, write, NULL, 0U);
    if (fd < 0)
        return -1;
    if (ld_fd_size_bytes(fd, capacity) != 0 || *capacity == 0U) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

int ldtm_raw_read(int fd, uint64_t offset, void *data, size_t bytes)
{
    return infiltratr_pread_full(fd, data, bytes, offset);
}
int ldtm_raw_write(int fd, uint64_t offset, const void *data, size_t bytes)
{
    return infiltratr_pwrite_full(fd, data, bytes, offset);
}

int ldtm_raw_plan(const LdtmFilesystemSpec *spec, uint64_t capacity, uint32_t unit, uint64_t first,
                  LdtmRawPlan *plan)
{
    if (spec == NULL || unit == 0U || capacity / unit <= first)
        return -1;
    *plan = (LdtmRawPlan){.capacity = capacity, .unit = unit};
    uint64_t cursor = first;
    for (unsigned phase = 0U; phase < 2U; ++phase) {
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            LdtmRawFile *item = &plan->files[file];
            item->bytes = ldtm_raw_file_bytes(spec, file);
            const uint64_t units = (item->bytes + unit - 1U) / unit;
            const uint64_t first_half = units > 1U ? units / 2U : units;
            const uint64_t count = phase == 0U ? first_half : units - first_half;
            item->start[phase] = count != 0U ? cursor : 0U;
            item->count[phase] = count;
            if (count == 0U)
                continue;
            if (cursor >= capacity / unit || count > capacity / unit - cursor)
                return -1;
            cursor += count + 2U;
        }
    }
    plan->end = cursor;
    return 0;
}

void ldtm_raw_payload(uint8_t *data, size_t bytes, unsigned file, uint64_t logical)
{
    /* Each 4 KiB logical page has its own seed. Verification can therefore
     * read arbitrary filesystem extents without retaining the original image
     * or restarting a volume-sized PRNG stream. The writer never owns this
     * oracle; it only relocates the opaque bytes manufactured here. */
    while (bytes != 0U) {
        const size_t within = (size_t)(logical % 4096U);
        size_t take = 4096U - within;
        if (take > bytes)
            take = bytes;
        uint64_t state = UINT64_C(0x243f6a8885a308d3) ^ ((uint64_t)file << 40U) ^
                         ((logical / 4096U) * UINT64_C(0x9e3779b97f4a7c15));
        for (size_t index = 0U; index < within + take; ++index) {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            state *= UINT64_C(0x2545f4914f6cdd1d);
            if (index >= within)
                data[index - within] = (uint8_t)(state >> 56U);
        }
        data += take;
        bytes -= take;
        logical += take;
    }
}

int ldtm_raw_write_payload(int fd, const LdtmRawPlan *plan)
{
    uint8_t buffer[65536];
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
        uint64_t logical = 0U;
        for (unsigned part = 0U; part < 2U; ++part) {
            uint64_t remaining = plan->files[file].count[part] * plan->unit;
            uint64_t physical = plan->files[file].start[part] * plan->unit;
            while (remaining != 0U) {
                const size_t take = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
                memset(buffer, 0, take);
                const uint64_t available =
                    logical < plan->files[file].bytes ? plan->files[file].bytes - logical : 0U;
                ldtm_raw_payload(buffer, available < take ? (size_t)available : take, file, logical);
                if (ldtm_raw_write(fd, physical, buffer, take) != 0)
                    return -1;
                logical += take;
                physical += take;
                remaining -= take;
            }
        }
    }
    return fsync(fd);
}

void ldtm_raw_check_init(LdtmRawCheck *check, const LdtmFilesystemSpec *spec)
{
    *check = (LdtmRawCheck){.spec = spec};
}
int ldtm_raw_check_file(LdtmRawCheck *check, unsigned file, uint64_t bytes)
{
    if (file >= LDTM_RAW_FILES || bytes != ldtm_raw_file_bytes(check->spec, file) ||
        (check->seen & (UINT32_C(1) << file)) != 0U)
        return -1;
    check->seen |= UINT32_C(1) << file;
    return 0;
}
int ldtm_raw_check_data(LdtmRawCheck *check, int fd, unsigned file, uint64_t physical,
                        uint64_t logical, uint64_t bytes)
{
    if (file >= LDTM_RAW_FILES || logical != check->done[file] ||
        logical > ldtm_raw_file_bytes(check->spec, file) ||
        bytes > ldtm_raw_file_bytes(check->spec, file) - logical)
        return -1;
    uint8_t actual[65536], expected[65536];
    while (bytes != 0U) {
        const size_t take = bytes < sizeof(actual) ? (size_t)bytes : sizeof(actual);
        if (ldtm_raw_read(fd, physical, actual, take) != 0)
            return -1;
        ldtm_raw_payload(expected, take, file, logical);
        if (memcmp(actual, expected, take) != 0)
            return -1;
        physical += take;
        logical += take;
        bytes -= take;
        check->done[file] += take;
    }
    return 0;
}
int ldtm_raw_check_finish(const LdtmRawCheck *check)
{
    if (check->seen != (UINT32_C(1) << LDTM_RAW_FILES) - 1U)
        return -1;
    for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file)
        if (check->done[file] != ldtm_raw_file_bytes(check->spec, file))
            return -1;
    return 0;
}
uint32_t ldtm_raw_crc32c(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < bytes; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0x82f63b78) : 0U);
    }
    return ~crc;
}

int ldtm_raw_fixture(const LdtmFilesystemSpec *spec, const char *path, bool create, char *detail,
                     size_t capacity)
{
    if (detail != NULL && capacity != 0U)
        detail[0] = '\0';
    if (spec == NULL || path == NULL)
        return -1;
    /* A missing/wrong module must be rejected before stale signatures are
     * erased. The privileged whole-disk worker also checks before partitioning. */
    if (create && (spec->creator == LDTM_CREATOR_FAT12 ||
                   spec->creator == LDTM_CREATOR_FAT16 ||
                   spec->creator == LDTM_CREATOR_FAT32) &&
        !ldtm_fat_module_available(spec)) {
        if (detail != NULL && capacity != 0U)
            (void)snprintf(detail, capacity, "Install or update the %s filesystem driver.", spec->key);
        return -1;
    }
    /* SWAP has no local formatter: refuse missing, untrusted or ABI-invalid
     * SWAP.so before opening any writable target or erasing signatures. */
    if (create && spec->creator == LDTM_CREATOR_SWAP &&
        !ldtm_swap_module_available(spec)) {
        if (detail && capacity)
            (void)snprintf(detail, capacity, "Install or update the swap filesystem driver.");
        return -1;
    }
    /* Missing/mismatched/untrusted ZFS.so must never erase existing media. */
    if (create && spec->creator == LDTM_CREATOR_ZFS &&
        !ldtm_zfs_module_available(spec)) {
        if (detail && capacity)
            (void)snprintf(detail,capacity,
              "Install or update the ZFS filesystem driver.");
        return -1;
    }
    if (create && spec->creator == LDTM_CREATOR_HFSPLUS &&
        !ldtm_hfsplus_module_available(spec)) {
        if (detail && capacity)
            (void)snprintf(detail,capacity,
                "Install or update the HFSPLUS.so filesystem driver.");
        return -1;
    }
    int (*codec)(const LdtmFilesystemSpec *, const char *, bool) = NULL;
    switch (spec->creator) {
    case LDTM_CREATOR_FAT12:
    case LDTM_CREATOR_FAT16:
    case LDTM_CREATOR_FAT32:
        codec = ldtm_raw_fat;
        break;
    case LDTM_CREATOR_EXFAT:
        codec = ldtm_raw_exfat;
        break;
    case LDTM_CREATOR_EXT2:
    case LDTM_CREATOR_EXT3:
    case LDTM_CREATOR_EXT4:
        codec = ldtm_raw_ext;
        break;
    case LDTM_CREATOR_MINIX:
        codec = ldtm_raw_minix;
        break;
    case LDTM_CREATOR_SWAP:
        codec = ldtm_raw_swap;
        break;
    case LDTM_CREATOR_HFS:
    case LDTM_CREATOR_HFSPLUS:
        codec = ldtm_raw_mac;
        break;
    case LDTM_CREATOR_NTFS:
        codec = ldtm_raw_ntfs;
        break;
    case LDTM_CREATOR_XFS:
        codec = ldtm_raw_xfs;
        break;
    case LDTM_CREATOR_BTRFS:
        codec = ldtm_raw_btrfs;
        break;
    case LDTM_CREATOR_ZFS:
        codec = ldtm_raw_zfs;
        break;
    default:
        break;
    }
    int result = -1;
    if (codec != NULL) {
        uint64_t bytes = 0U;
        const int fd = ldtm_raw_open(path, create, &bytes);
        if (fd >= 0) {
            bool valid = bytes == (uint64_t)spec->size_mib * LDTM_MIB;
            if (create && spec->creator != LDTM_CREATOR_FAT12 &&
                spec->creator != LDTM_CREATOR_FAT16) {
                uint8_t zero[65536] = {0};
                /* Erase stale primary signatures and all four ZFS label rings
                 * before reusing a sacrificial slot. No format driver runs. */
                for (uint64_t offset = 0U; valid && offset < LDTM_MIB; offset += sizeof(zero))
                    valid = ldtm_raw_write(fd, offset, zero, sizeof(zero)) == 0 &&
                            ldtm_raw_write(fd, bytes - LDTM_MIB + offset, zero, sizeof(zero)) == 0;
            }
            if (close(fd) != 0)
                valid = false;
            if (valid)
                result = codec(spec, path, create);
        }
    }
    if (detail != NULL && capacity != 0U)
        (void)snprintf(detail, capacity, "%s native %s %s", spec->key,
                       create ? "fixture creation" : "complete corpus verification",
                       result == 0 ? "passed" : "failed");
    return result;
}
