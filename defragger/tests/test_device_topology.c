// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_device.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int read_device_number(const char *path, dev_t *device)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) return -1;

    unsigned major_number = 0U;
    unsigned minor_number = 0U;
    const int matched = fscanf(file, "%u:%u", &major_number, &minor_number);
    const int close_result = fclose(file);
    if (matched != 2 || close_result != 0) {
        if (errno == 0) errno = EIO;
        return -1;
    }
    *device = makedev(major_number, minor_number);
    return 0;
}

int main(void)
{
    DIR *directory = opendir("/sys/class/block");
    if (directory == NULL) {
        perror("open /sys/class/block");
        return 1;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char partition_path[PATH_MAX];
        char device_path[PATH_MAX];
        const int partition_length = snprintf(
            partition_path, sizeof(partition_path),
            "/sys/class/block/%s/partition", entry->d_name);
        const int device_length = snprintf(
            device_path, sizeof(device_path),
            "/sys/class/block/%s/dev", entry->d_name);
        if (partition_length < 0 || device_length < 0 ||
            (size_t)partition_length >= sizeof(partition_path) ||
            (size_t)device_length >= sizeof(device_path)) {
            closedir(directory);
            fprintf(stderr, "sysfs partition path exceeds PATH_MAX\n");
            return 1;
        }
        if (access(partition_path, F_OK) != 0) {
            if (errno == ENOENT) continue;
            const int failure = errno;
            closedir(directory);
            errno = failure;
            perror("inspect sysfs partition attribute");
            return 1;
        }

        dev_t device = 0;
        if (read_device_number(device_path, &device) != 0) {
            const int failure = errno;
            closedir(directory);
            errno = failure;
            perror("read sysfs partition device number");
            return 1;
        }
        closedir(directory);

        /*
         * This is deliberately a black-box platform-boundary regression. The
         * result may be true or false depending on the runner's mount namespace;
         * the contract under test is that an ordinary Linux partition has a
         * valid topology and therefore returns normally instead of aborting on
         * a non-existent partition slaves/ directory.
         */
        const bool mounted = ld_device_number_is_mounted(device);
        printf("partition topology accepted for %u:%u (mounted=%s)\n",
               major(device), minor(device), mounted ? "true" : "false");
        return 0;
    }

    closedir(directory);
    fprintf(stderr, "SKIP: runner exposes no Linux partition nodes\n");
    return 77;
}
