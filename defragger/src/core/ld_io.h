// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_IO_H
#define LD_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Defragmenter I/O adapters.
 *
 * Common owns the interruption-safe exact positioned-I/O loop. These wrappers
 * adapt that primitive to the ssize_t/fatal-policy contracts used by the native
 * engines. A non-negative return equals the requested length; -1 indicates
 * failure and preserves errno for diagnostics.
 */
ssize_t ld_pread_full(int fd, void *buffer, size_t length, uint64_t offset);
ssize_t ld_pwrite_full(int fd, const void *buffer, size_t length, uint64_t offset);

/*
 * Packed boolean allocation maps.  Filesystem engines frequently need one
 * yes/no state per allocation unit; storing one byte per unit wastes 8x the
 * memory on large media.  These helpers keep that representation consistent
 * without moving filesystem policy into the core.
 */
bool ld_bitmap_size(uint64_t bits, size_t *bytes);
uint8_t *ld_bitmap_calloc(uint64_t bits);
bool ld_bitmap_get(const uint8_t *bitmap, uint64_t bit);
void ld_bitmap_set(uint8_t *bitmap, uint64_t bit, bool value);

/* Retry fsync() across EINTR so transaction boundaries share one primitive. */
int ld_sync_fd(int fd);

/* Fatal variants for invariants whose violation cannot be recovered locally. */
void ld_pread_exact(int fd, void *buffer, size_t length, uint64_t offset,
                    const char *what);
void ld_pwrite_exact(int fd, const void *buffer, size_t length, uint64_t offset,
                     const char *what);

/*
 * Resource-selection helpers are best-effort performance policy, not safety
 * boundaries. Writers must remain correct with smaller resources and fail
 * closed when a required workspace cannot be obtained.
 */
uint64_t ld_available_memory_bytes(void);
size_t ld_default_ram_limit(void);
size_t ld_online_cpu_count(void);
size_t ld_default_worker_count(bool rotational, bool serial_flash);

#endif
