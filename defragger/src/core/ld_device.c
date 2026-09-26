// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_device.h"
#include "ld_runtime.h"

#include "infiltratr/core.h"
#include "infiltratr/posix.h"
#include "infiltratr/posix_path.h"
#include "infiltratr/token.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

enum { LD_MAX_RELATED_DEVICES = 1024 };

typedef struct {
    dev_t value;
    bool exact_mapping;
    bool relations_scanned;
    bool children_scanned;
} LdRelatedDevice;

static bool ld_add_related_device(LdRelatedDevice *devices, size_t *count,
                                  dev_t value, bool exact_mapping) {
    for (size_t index = 0; index < *count; ++index) {
        if (devices[index].value != value) continue;
        if (exact_mapping) devices[index].exact_mapping = true;
        return true;
    }
    if (*count >= LD_MAX_RELATED_DEVICES) return false;
    devices[*count] = (LdRelatedDevice){
        .value = value,
        .exact_mapping = exact_mapping,
        .relations_scanned = false,
        .children_scanned = false,
    };
    (*count)++;
    return true;
}

static bool ld_parse_device_number(const char *text, dev_t *result) {
    if (text == NULL || result == NULL) return false;
    const char *cursor = text;
    uint64_t found_major = 0U;
    uint64_t found_minor = 0U;
    if (!infiltratr_parse_u64_token(&cursor, 10U, &found_major) ||
        found_major > UINT_MAX || *cursor != ':')
        return false;
    cursor++;
    if (!infiltratr_parse_u64_token(&cursor, 10U, &found_minor) ||
        found_minor > UINT_MAX || *cursor != '\0')
        return false;
    *result = makedev((unsigned)found_major, (unsigned)found_minor);
    return true;
}

static bool ld_read_sysfs_device(const char *path, dev_t *result) {
    char text[64];
    return infiltratr_read_text_file(path, text, sizeof(text)) &&
           ld_parse_device_number(text, result);
}

static bool ld_resolve_sysfs_device(dev_t device, char *path, size_t size) {
    char link_path[PATH_MAX];
    int length = snprintf(link_path, sizeof(link_path), "/sys/dev/block/%u:%u",
                          major(device), minor(device));
    if (length < 0 || (size_t)length >= sizeof(link_path)) return false;
    return infiltratr_realpath_copy(link_path, path, size);
}

static bool ld_collect_sysfs_directory(const char *path,
                                       LdRelatedDevice *devices,
                                       size_t *count, bool exact_mapping) {
    DIR *directory = opendir(path);
    if (directory == NULL) return true;
    struct dirent *entry = NULL;
    bool complete = true;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char device_path[PATH_MAX];
        int length = snprintf(device_path, sizeof(device_path), "%s/%s/dev",
                              path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(device_path)) {
            complete = false;
            break;
        }
        dev_t related = 0;
        if (ld_read_sysfs_device(device_path, &related) &&
            !ld_add_related_device(devices, count, related, exact_mapping)) {
            complete = false;
            break;
        }
    }
    closedir(directory);
    return complete;
}

static bool ld_add_parent_device(const char *sysfs_path,
                                 LdRelatedDevice *devices, size_t *count) {
    char parent_path[PATH_MAX];
    int length = snprintf(parent_path, sizeof(parent_path), "%s", sysfs_path);
    if (length < 0 || (size_t)length >= sizeof(parent_path)) return false;
    char *separator = strrchr(parent_path, '/');
    if (separator == NULL) return true;
    *separator = '\0';
    size_t used = strlen(parent_path);
    if (used + sizeof("/dev") > sizeof(parent_path)) return false;
    memcpy(parent_path + used, "/dev", sizeof("/dev"));
    dev_t parent = 0;
    if (ld_read_sysfs_device(parent_path, &parent) &&
        !ld_add_related_device(devices, count, parent, false)) return false;
    return true;
}

static bool ld_collect_related_devices(dev_t target,
                                       LdRelatedDevice *devices,
                                       size_t *count) {
    /*
     * exact_mapping=true means this node itself maps the selected storage
     * range.  Child partitions of such a whole-device mapping overlap it and
     * must be checked.  A parent device that merely contains the selected
     * partition is exact_mapping=false: the parent itself overlaps the target,
     * but its sibling partitions do not.
     */
    if (!ld_add_related_device(devices, count, target, true)) return false;

    for (size_t index = 0; index < *count; ++index) {
        char sysfs_path[PATH_MAX];
        if (!ld_resolve_sysfs_device(devices[index].value,
                                     sysfs_path, sizeof(sysfs_path))) continue;

        if (!devices[index].relations_scanned) {
            devices[index].relations_scanned = true;
            if (!ld_add_parent_device(sysfs_path, devices, count)) return false;

            char relation_path[PATH_MAX];
            for (size_t relation = 0; relation < 2; ++relation) {
                const char *name = relation == 0 ? "holders" : "slaves";
                int length = snprintf(relation_path, sizeof(relation_path), "%s/%s",
                                      sysfs_path, name);
                if (length < 0 || (size_t)length >= sizeof(relation_path)) return false;
                if (!ld_collect_sysfs_directory(relation_path, devices, count, true))
                    return false;
            }
        }

        if (devices[index].exact_mapping && !devices[index].children_scanned) {
            devices[index].children_scanned = true;
            if (!ld_collect_sysfs_directory(sysfs_path, devices, count, true))
                return false;
        }
    }
    return true;
}

static bool ld_related_contains(const LdRelatedDevice *devices,
                                size_t count, dev_t value) {
    for (size_t index = 0U; index < count; ++index)
        if (devices[index].value == value) return true;
    return false;
}

static bool ld_mount_target_is_user_media(const char *target) {
    return target != NULL &&
           (strcmp(target, "/mnt") == 0 ||
            infiltratr_string_starts_with(target, "/mnt/") ||
            infiltratr_string_starts_with(target, "/media/") ||
            infiltratr_string_starts_with(target, "/run/media/"));
}

bool ld_device_number_is_mounted(dev_t device_number) {
    LdRelatedDevice related[LD_MAX_RELATED_DEVICES];
    size_t related_count = 0;
    if (!ld_collect_related_devices(device_number, related, &related_count))
        ld_die("block-device topology is too large to validate safely");

    FILE *file = fopen("/proc/self/mountinfo", "r");
    if (file == NULL) ld_die_errno("open /proc/self/mountinfo");
    char *line = NULL;
    size_t capacity = 0;
    bool mounted = false;
    while (getline(&line, &capacity, file) >= 0) {
        char *cursor = line;
        int field = 0;
        while (*cursor != '\0') {
            while (*cursor == ' ') cursor++;
            if (*cursor == '\0') break;
            field++;
            char *end = strchr(cursor, ' ');
            if (field == 3) {
                const char saved = end == NULL ? '\0' : *end;
                if (end != NULL) *end = '\0';
                dev_t found = 0;
                const bool valid = ld_parse_device_number(cursor, &found);
                if (end != NULL) *end = saved;
                if (valid) {
                    for (size_t index = 0; index < related_count; ++index) {
                        if (related[index].value == found) {
                            mounted = true;
                            break;
                        }
                    }
                    if (mounted) break;
                }
            }
            if (end == NULL) break;
            cursor = end + 1;
        }
        if (mounted) break;
    }
    free(line);
    fclose(file);
    return mounted;
}

static bool ld_sysfs_text(const char *sysfs, const char *suffix,
                          char *output, size_t output_size) {
    char path[PATH_MAX];
    const int length = snprintf(path, sizeof(path), "%s/%s", sysfs, suffix);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;
    return infiltratr_read_text_file(path, output, output_size);
}

static void ld_read_udev_properties(dev_t device, LdBlockDeviceInfo *info) {
    char path[PATH_MAX];
    const int length = snprintf(path, sizeof(path), "/run/udev/data/b%u:%u",
                                major(device), minor(device));
    if (length < 0 || (size_t)length >= sizeof(path)) return;

    FILE *file = fopen(path, "r");
    if (file == NULL) return;
    char *line = NULL;
    size_t capacity = 0U;
    while (getline(&line, &capacity, file) >= 0) {
        infiltratr_trim_line_end(line);
        const char *value = NULL;
        if (infiltratr_string_starts_with(line, "E:ID_SERIAL_SHORT="))
            value = line + strlen("E:ID_SERIAL_SHORT=");
        else if (info->serial[0] == '\0' &&
                 infiltratr_string_starts_with(line, "E:ID_SERIAL="))
            value = line + strlen("E:ID_SERIAL=");
        else if (infiltratr_string_starts_with(line, "E:ID_WWN="))
            value = line + strlen("E:ID_WWN=");
        else if (infiltratr_string_starts_with(line, "E:ID_BUS="))
            value = line + strlen("E:ID_BUS=");
        else
            continue;

        if (infiltratr_string_starts_with(line, "E:ID_WWN="))
            infiltratr_copy_string(info->wwn, sizeof(info->wwn), value);
        else if (infiltratr_string_starts_with(line, "E:ID_BUS="))
            infiltratr_copy_string(info->transport, sizeof(info->transport), value);
        else
            infiltratr_copy_string(info->serial, sizeof(info->serial), value);
    }
    free(line);
    (void)fclose(file);
}

int ld_block_device_info(const char *path, LdBlockDeviceInfo *info) {
    if (path == NULL || info == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(info, 0, sizeof(*info));

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISBLK(st.st_mode)) {
        errno = ENOTBLK;
        return -1;
    }

    char sysfs[PATH_MAX];
    if (!ld_resolve_sysfs_device(st.st_rdev, sysfs, sizeof(sysfs))) {
        errno = ENODEV;
        return -1;
    }

    char partition_path[PATH_MAX];
    int length = snprintf(partition_path, sizeof(partition_path),
                          "%s/partition", sysfs);
    if (length < 0 || (size_t)length >= sizeof(partition_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (access(partition_path, F_OK) == 0) {
        info->whole_disk = false;
    } else if (errno == ENOENT) {
        info->whole_disk = true;
    } else {
        return -1;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    const int size_result = ld_fd_size_bytes(fd, &info->size_bytes);
    const int size_error = errno;
    (void)close(fd);
    if (size_result != 0) {
        errno = size_error;
        return -1;
    }

    char numeric_path[PATH_MAX];
    uint64_t value = 0U;
    length = snprintf(numeric_path, sizeof(numeric_path), "%s/removable", sysfs);
    if (length < 0 || (size_t)length >= sizeof(numeric_path) ||
        !infiltratr_read_u64_file(numeric_path, &value) || value > 1U) {
        errno = EIO;
        return -1;
    }
    info->removable = value != 0U;

    value = 0U;
    length = snprintf(numeric_path, sizeof(numeric_path), "%s/ro", sysfs);
    if (length < 0 || (size_t)length >= sizeof(numeric_path) ||
        !infiltratr_read_u64_file(numeric_path, &value) || value > 1U) {
        errno = EIO;
        return -1;
    }
    info->read_only = value != 0U;

    (void)ld_sysfs_text(sysfs, "device/model", info->model, sizeof(info->model));
    (void)ld_sysfs_text(sysfs, "device/serial", info->serial, sizeof(info->serial));
    if (!ld_sysfs_text(sysfs, "device/wwid", info->wwn, sizeof(info->wwn)))
        (void)ld_sysfs_text(sysfs, "wwid", info->wwn, sizeof(info->wwn));

    ld_read_udev_properties(st.st_rdev, info);

    if (info->transport[0] == '\0') {
        const char *base = infiltratr_path_basename(path);
        const char *transport = "";
        if (strstr(sysfs, "/usb") != NULL) transport = "usb";
        else if (base != NULL && infiltratr_string_starts_with(base, "nvme"))
            transport = "nvme";
        else if (base != NULL && infiltratr_string_starts_with(base, "mmcblk"))
            transport = "mmc";
        else if (strstr(sysfs, "/ata") != NULL) transport = "sata";
        else if (strstr(sysfs, "/virtio") != NULL) transport = "virtio";
        else if (strstr(sysfs, "/scsi") != NULL) transport = "scsi";
        infiltratr_copy_string(info->transport, sizeof(info->transport), transport);
    }
    return 0;
}

int ld_block_device_has_system_use(const char *path, bool *in_use) {
    if (path == NULL || in_use == NULL) {
        errno = EINVAL;
        return -1;
    }
    *in_use = true;

    struct stat target;
    if (stat(path, &target) != 0)
        return -1;
    if (!S_ISBLK(target.st_mode)) {
        errno = ENOTBLK;
        return -1;
    }

    LdRelatedDevice related[LD_MAX_RELATED_DEVICES];
    size_t related_count = 0U;
    if (!ld_collect_related_devices(target.st_rdev, related, &related_count)) {
        errno = EOVERFLOW;
        return -1;
    }

    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (mounts == NULL) return -1;
    char *line = NULL;
    size_t capacity = 0U;
    while (getline(&line, &capacity, mounts) >= 0) {
        char device_text[64] = "";
        char mountpoint[PATH_MAX] = "";
        if (sscanf(line, "%*s %*s %63s %*s %4095s",
                   device_text, mountpoint) != 2)
            continue;
        dev_t mounted_device = 0;
        if (ld_parse_device_number(device_text, &mounted_device) &&
            ld_related_contains(related, related_count, mounted_device) &&
            !ld_mount_target_is_user_media(mountpoint)) {
            free(line);
            fclose(mounts);
            *in_use = true;
            return 0;
        }
    }
    free(line);
    if (ferror(mounts)) {
        const int failure = errno == 0 ? EIO : errno;
        fclose(mounts);
        errno = failure;
        return -1;
    }
    fclose(mounts);

    FILE *swaps = fopen("/proc/swaps", "r");
    if (swaps == NULL) return -1;
    char swap_line[PATH_MAX + 256U];
    if (fgets(swap_line, sizeof(swap_line), swaps) == NULL) {
        fclose(swaps);
        errno = EIO;
        return -1;
    }
    while (fgets(swap_line, sizeof(swap_line), swaps) != NULL) {
        char source[PATH_MAX];
        if (sscanf(swap_line, "%4095s", source) != 1) continue;
        struct stat swap_stat;
        if (stat(source, &swap_stat) != 0) {
            fclose(swaps);
            return -1;
        }
        const dev_t device = S_ISBLK(swap_stat.st_mode)
            ? swap_stat.st_rdev : swap_stat.st_dev;
        if (ld_related_contains(related, related_count, device)) {
            fclose(swaps);
            *in_use = true;
            return 0;
        }
    }
    if (ferror(swaps)) {
        const int failure = errno == 0 ? EIO : errno;
        fclose(swaps);
        errno = failure;
        return -1;
    }
    fclose(swaps);
    *in_use = false;
    return 0;
}

static void ld_decode_mount_field(char *value) {
    if (value == NULL) return;
    char *read_cursor = value;
    char *write_cursor = value;
    while (*read_cursor != '\0') {
        if (read_cursor[0] == '\\' &&
            read_cursor[1] != '\0' && read_cursor[2] != '\0' &&
            read_cursor[3] != '\0' &&
            read_cursor[1] >= '0' && read_cursor[1] <= '7' &&
            read_cursor[2] >= '0' && read_cursor[2] <= '7' &&
            read_cursor[3] >= '0' && read_cursor[3] <= '7') {
            const unsigned decoded =
                (unsigned)(read_cursor[1] - '0') * 64U +
                (unsigned)(read_cursor[2] - '0') * 8U +
                (unsigned)(read_cursor[3] - '0');
            *write_cursor++ = (char)decoded;
            read_cursor += 4;
        } else {
            *write_cursor++ = *read_cursor++;
        }
    }
    *write_cursor = '\0';
}

static bool ld_mount_source_matches_regular_file(const char *real_path) {
    FILE *file = fopen("/proc/self/mountinfo", "r");
    if (file == NULL) ld_die_errno("open /proc/self/mountinfo");

    char *line = NULL;
    size_t capacity = 0U;
    bool mounted = false;
    while (getline(&line, &capacity, file) >= 0) {
        char *separator = strstr(line, " - ");
        if (separator == NULL) continue;
        char *cursor = separator + 3;
        char *filesystem_end = strchr(cursor, ' ');
        if (filesystem_end == NULL) continue;
        cursor = filesystem_end + 1;
        char *source_end = strchr(cursor, ' ');
        if (source_end == NULL) continue;
        *source_end = '\0';
        ld_decode_mount_field(cursor);

        struct stat source, target;
        mounted = stat(cursor, &source) == 0 &&
                  stat(real_path, &target) == 0 &&
                  S_ISREG(source.st_mode) &&
                  source.st_dev == target.st_dev &&
                  source.st_ino == target.st_ino;
        *source_end = ' ';
        if (mounted) break;
    }
    free(line);
    fclose(file);
    return mounted;
}

static bool ld_loop_backing_file_matches(const char *sysfs_path,
                                         const char *real_path) {
    char backing_path[PATH_MAX];
    int length = snprintf(backing_path, sizeof(backing_path),
                          "%s/loop/backing_file", sysfs_path);
    if (length < 0 || (size_t)length >= sizeof(backing_path)) return false;

    char raw[PATH_MAX];
    if (!infiltratr_read_text_file(backing_path, raw, sizeof(raw))) return false;
    size_t raw_length = strlen(raw);
    while (raw_length > 0U &&
           (raw[raw_length - 1U] == '\n' || raw[raw_length - 1U] == '\r')) {
        raw[--raw_length] = '\0';
    }
    if (raw_length == 0U) return false;

    char candidate[PATH_MAX];
    if (raw[0] == '/') {
        length = snprintf(candidate, sizeof(candidate), "%s", raw);
    } else {
        length = snprintf(candidate, sizeof(candidate), "/%s", raw);
    }
    if (length < 0 || (size_t)length >= sizeof(candidate)) return false;

    /* Canonical path strings do not identify hard links. Bind the loop's
     * backing object to the selected inode, just as the verified open does. */
    struct stat backing, target;
    return stat(candidate, &backing) == 0 &&
           stat(real_path, &target) == 0 && S_ISREG(backing.st_mode) &&
           backing.st_dev == target.st_dev && backing.st_ino == target.st_ino;
}

static bool ld_regular_file_loop_is_mounted(const char *real_path) {
    DIR *directory = opendir("/sys/dev/block");
    if (directory == NULL) return false;

    bool mounted = false;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        dev_t device = 0;
        if (!ld_parse_device_number(entry->d_name, &device)) continue;

        char sysfs_path[PATH_MAX];
        if (!ld_resolve_sysfs_device(device, sysfs_path, sizeof(sysfs_path)))
            continue;
        if (ld_loop_backing_file_matches(sysfs_path, real_path) &&
            ld_device_number_is_mounted(device)) {
            mounted = true;
            break;
        }
    }
    closedir(directory);
    return mounted;
}

bool ld_path_is_mounted(const char *path) {
    if (path == NULL) {
        errno = EINVAL;
        ld_die_errno("stat device");
    }

    char *resolved = realpath(path, NULL);
    if (resolved == NULL) ld_die_errno("realpath target");

    struct stat status;
    if (stat(resolved, &status) != 0) {
        const int failure = errno;
        free(resolved);
        errno = failure;
        ld_die_errno("stat device");
    }

    bool mounted = false;
    if (S_ISBLK(status.st_mode)) {
        mounted = ld_device_number_is_mounted(status.st_rdev);
    } else if (S_ISREG(status.st_mode)) {
        mounted = ld_mount_source_matches_regular_file(resolved) ||
                  ld_regular_file_loop_is_mounted(resolved);
    }

    free(resolved);
    return mounted;
}

/*
 * The validation is intentionally repeated across the open boundary.
 *
 * A pathname is not a stable authority: another process can replace a regular
 * image or change block-device mount state after preflight. Therefore this
 * routine resolves and stats the candidate, rejects mounted writable block
 * targets, opens with no-follow/exclusive semantics where applicable, compares
 * fstat() identity with the pre-open object, then repeats the mounted-state
 * check on the descriptor's block identity. Filesystem writers add their
 * UUID/serial/geometry checks on top of this generic object binding.
 */
int ld_device_try_open(const char *path, bool writable, LdDevice *device) {
    if (path == NULL || device == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(device, 0, sizeof(*device));
    device->fd = -1;

    char *resolved = realpath(path, NULL);
    if (resolved == NULL) return -1;

    struct stat expected;
    if (stat(resolved, &expected) != 0) {
        const int failure = errno;
        free(resolved);
        errno = failure;
        return -1;
    }
    const bool block = S_ISBLK(expected.st_mode);
    if (!block && !S_ISREG(expected.st_mode)) {
        free(resolved);
        errno = EINVAL;
        return -1;
    }
    if (block && writable && ld_device_number_is_mounted(expected.st_rdev)) {
        free(resolved);
        errno = EBUSY;
        return -1;
    }

    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (block && writable) flags |= O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(resolved, flags);
    if (fd < 0) {
        const int failure = errno;
        free(resolved);
        errno = failure;
        return -1;
    }

    struct stat opened;
    if (fstat(fd, &opened) != 0) {
        const int failure = errno;
        (void)close(fd);
        free(resolved);
        errno = failure;
        return -1;
    }
    const bool same_kind =
        (expected.st_mode & S_IFMT) == (opened.st_mode & S_IFMT);
    const bool same_object =
        same_kind && expected.st_dev == opened.st_dev &&
        expected.st_ino == opened.st_ino &&
        (!block || expected.st_rdev == opened.st_rdev);
    if (!same_object) {
        (void)close(fd);
        free(resolved);
        errno = ESTALE;
        return -1;
    }
    if (block && writable && ld_device_number_is_mounted(opened.st_rdev)) {
        (void)close(fd);
        free(resolved);
        errno = EBUSY;
        return -1;
    }

    uint64_t size = 0;
    if (ld_fd_size_bytes(fd, &size) != 0) {
        const int failure = errno;
        (void)close(fd);
        free(resolved);
        errno = failure;
        return -1;
    }

    *device = (LdDevice){
        .fd = fd,
        .path = resolved,
        .writable = writable,
        .is_block = block,
        .size_bytes = size,
        .device_number = block ? opened.st_rdev : 0,
        .host_device = opened.st_dev,
        .inode = opened.st_ino,
    };
    return 0;
}

int ld_fd_size_bytes(int fd, uint64_t *size_bytes) {
    if (fd < 0 || size_bytes == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct stat status;
    if (fstat(fd, &status) != 0) return -1;
    if (S_ISREG(status.st_mode)) {
        if (status.st_size < 0) {
            errno = EOVERFLOW;
            return -1;
        }
        *size_bytes = (uint64_t)status.st_size;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, BLKGETSIZE64, size_bytes);
}

int ld_device_format_identity(const LdDevice *device,
                              char *buffer, size_t buffer_size) {
    if (device == NULL || device->fd < 0 || buffer == NULL ||
        buffer_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    int written = 0;
    if (device->is_block) {
        written = snprintf(buffer, buffer_size, "block:%u:%u",
                           major(device->device_number),
                           minor(device->device_number));
    } else {
        written = snprintf(buffer, buffer_size, "file:%llu:%llu",
                           (unsigned long long)device->host_device,
                           (unsigned long long)device->inode);
    }
    if (written < 0) {
        errno = EIO;
        return -1;
    }
    if ((size_t)written >= buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int ld_fd_format_identity(int fd, char *buffer, size_t buffer_size) {
    if (fd < 0 || buffer == NULL || buffer_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    struct stat status;
    if (fstat(fd, &status) != 0) return -1;
    const bool block = S_ISBLK(status.st_mode);
    if (!block && !S_ISREG(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    const LdDevice snapshot = {
        .fd = fd,
        .path = NULL,
        .writable = false,
        .is_block = block,
        .size_bytes = 0U,
        .device_number = block ? status.st_rdev : 0,
        .host_device = status.st_dev,
        .inode = status.st_ino,
    };
    return ld_device_format_identity(&snapshot, buffer, buffer_size);
}

bool ld_device_matches_identity(const LdDevice *device,
                                const char *expected_identity,
                                uint64_t expected_size) {
    if (device == NULL || device->fd < 0 || expected_identity == NULL)
        return false;
    if (expected_size != 0U && device->size_bytes != expected_size)
        return false;
    char identity[160];
    return ld_device_format_identity(device, identity, sizeof(identity)) == 0 &&
           strcmp(identity, expected_identity) == 0;
}

bool ld_fd_matches_identity(int fd, const char *expected_identity,
                            uint64_t expected_size) {
    if (fd < 0 || expected_identity == NULL) return false;
    uint64_t size = 0U;
    if (ld_fd_size_bytes(fd, &size) != 0) return false;
    if (expected_size != 0U && size != expected_size) return false;

    char identity[160];
    return ld_fd_format_identity(fd, identity, sizeof(identity)) == 0 &&
           strcmp(identity, expected_identity) == 0;
}

int ld_device_open_verified_fd(const char *path, bool writable,
                               const char *expected_identity,
                               uint64_t expected_size) {
    LdDevice device;
    if (ld_device_try_open(path, writable, &device) != 0)
        return -1;
    if (expected_identity != NULL &&
        !ld_device_matches_identity(&device, expected_identity, expected_size)) {
        ld_device_close(&device);
        errno = ESTALE;
        return -1;
    }
    const int fd = device.fd;
    device.fd = -1;
    ld_device_close(&device);
    return fd;
}

int ld_device_capture_binding(const char *path, char **canonical_path,
                              char **identity, uint64_t *size_bytes) {
    if (path == NULL || canonical_path == NULL || identity == NULL ||
        size_bytes == NULL) {
        errno = EINVAL;
        return -1;
    }
    *canonical_path = NULL;
    *identity = NULL;
    *size_bytes = 0U;

    LdDevice device;
    if (ld_device_try_open(path, false, &device) != 0)
        return -1;
    if (device.size_bytes == 0U) {
        ld_device_close(&device);
        errno = EINVAL;
        return -1;
    }

    char text[160];
    if (ld_device_format_identity(&device, text, sizeof(text)) != 0) {
        const int failure = errno;
        ld_device_close(&device);
        errno = failure;
        return -1;
    }
    char *copied_identity = strdup(text);
    if (copied_identity == NULL) {
        const int failure = errno;
        ld_device_close(&device);
        errno = failure;
        return -1;
    }

    *canonical_path = device.path;
    device.path = NULL;
    *identity = copied_identity;
    *size_bytes = device.size_bytes;
    ld_device_close(&device);
    return 0;
}

LdDevice ld_device_open(const char *path, bool writable) {
    LdDevice device;
    if (ld_device_try_open(path, writable, &device) != 0)
        ld_die_errno("open target");
    return device;
}

void ld_device_close(LdDevice *device) {
    if (device == NULL) return;
    if (device->fd >= 0 && close(device->fd) != 0) ld_warn_errno("close target");
    free(device->path);
    memset(device, 0, sizeof(*device));
    device->fd = -1;
}

bool ld_device_is_rotational(const LdDevice *device) {
    if (!device->is_block) return false;
    char path[128];
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/queue/rotational",
             major(device->device_number), minor(device->device_number));
    uint64_t value = 0;
    return infiltratr_read_u64_file(path, &value) && value != 0;
}

bool ld_device_is_serial_flash(const LdDevice *device) {
    return infiltratr_string_starts_with(
        infiltratr_path_basename(device->path), "mmcblk");
}
