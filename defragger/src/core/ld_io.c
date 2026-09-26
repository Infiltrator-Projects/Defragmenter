// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_io.h"
#include "ld_runtime.h"

#include "infiltratr/posix_io.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t ld_pread_full(int fd, void *buffer, size_t length, uint64_t offset) {
    if (length > (size_t)SSIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return infiltratr_pread_full(fd, buffer, length, offset) == 0
        ? (ssize_t)length : -1;
}

ssize_t ld_pwrite_full(int fd, const void *buffer, size_t length, uint64_t offset) {
    if (length > (size_t)SSIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return infiltratr_pwrite_full(fd, buffer, length, offset) == 0
        ? (ssize_t)length : -1;
}

bool ld_bitmap_size(uint64_t bits, size_t *bytes) {
    if (bytes == NULL || bits > UINT64_MAX - UINT64_C(7)) {
        errno = EOVERFLOW;
        return false;
    }
    const uint64_t packed = (bits + UINT64_C(7)) / UINT64_C(8);
    if (packed > SIZE_MAX) {
        errno = EOVERFLOW;
        return false;
    }
    *bytes = (size_t)packed;
    return true;
}

uint8_t *ld_bitmap_calloc(uint64_t bits) {
    size_t bytes = 0U;
    if (!ld_bitmap_size(bits, &bytes)) return NULL;
    return calloc(bytes == 0U ? 1U : bytes, 1U);
}

bool ld_bitmap_get(const uint8_t *bitmap, uint64_t bit) {
    return bitmap != NULL &&
           (bitmap[bit >> 3U] &
            (uint8_t)(1U << (unsigned int)(bit & UINT64_C(7)))) != 0U;
}

void ld_bitmap_set(uint8_t *bitmap, uint64_t bit, bool value) {
    const uint8_t mask =
        (uint8_t)(1U << (unsigned int)(bit & UINT64_C(7)));
    if (value)
        bitmap[bit >> 3U] |= mask;
    else
        bitmap[bit >> 3U] &= (uint8_t)~mask;
}

int ld_sync_fd(int fd) {
    int result;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result;
}

void ld_pread_exact(int fd, void *buffer, size_t length, uint64_t offset, const char *what) {
    ssize_t count = ld_pread_full(fd, buffer, length, offset);
    if (count < 0) ld_die_errno(what);
    if ((size_t)count != length) {
        errno = EIO;
        ld_die_errno(what);
    }
}

void ld_pwrite_exact(int fd, const void *buffer, size_t length, uint64_t offset, const char *what) {
    ssize_t count = ld_pwrite_full(fd, buffer, length, offset);
    if (count < 0 || (size_t)count != length) {
        if (count >= 0) errno = EIO;
        ld_die_errno(what);
    }
}

uint64_t ld_available_memory_bytes(void) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file != NULL) {
        char key[64];
        unsigned long long value = 0;
        char unit[16];
        while (fscanf(file, "%63s %llu %15s", key, &value, unit) == 3) {
            if (strcmp(key, "MemAvailable:") == 0) {
                fclose(file);
                return (uint64_t)value * UINT64_C(1024);
            }
        }
        fclose(file);
    }
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0)
        return (uint64_t)(unsigned long)pages * (uint64_t)(unsigned long)page_size;
    return UINT64_C(512) * 1024 * 1024;
}

size_t ld_default_ram_limit(void) {
    uint64_t available = ld_available_memory_bytes();
    const uint64_t minimum = UINT64_C(64) * 1024 * 1024;
    const uint64_t reserve = UINT64_C(8) * 1024 * 1024 * 1024;
    const uint64_t maximum = UINT64_C(16) * 1024 * 1024 * 1024;
    uint64_t selected = available > reserve + minimum ? available - reserve : available / 4;
    if (selected < minimum) selected = minimum;
    if (selected > maximum) selected = maximum;
    if (selected > SIZE_MAX) selected = SIZE_MAX;
    return (size_t)selected;
}

size_t ld_online_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (size_t)count : 1;
}

size_t ld_default_worker_count(bool rotational, bool serial_flash) {
    const size_t cpus = ld_online_cpu_count();
    const size_t cpu_budget = cpus > 1U ? cpus - 1U : 1U;

    /*
     * Parallel-capable work leaves one logical processor available to the
     * desktop. Storage media may impose a lower useful I/O concurrency, but
     * they never raise the CPU ceiling above N-1.
     */
    if (rotational) return 1U;
    if (serial_flash) return cpu_budget > 2U ? 2U : cpu_budget;
    return cpu_budget;
}
