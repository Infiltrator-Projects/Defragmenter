// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_DEVICE_H
#define LD_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include <sys/types.h>

/*
 * Filesystem-neutral target identity and mounted-overlap safety.
 *
 * Ownership: successful ld_device_try_open()/ld_device_open() calls transfer an
 * open descriptor and an allocated canonical path to LdDevice; ld_device_close()
 * releases both. Writable block-device opens are deliberately fail-closed:
 * the target is checked for mounted overlap before and after open, the final
 * path component is not followed, and the opened descriptor must still name
 * the object inspected before open.
 *
 * Identity strings bind block devices by major:minor and regular images by host
 * device:inode. expected_size, when non-zero, adds a capacity check. These
 * checks defend against pathname replacement; filesystem-specific UUID/serial/
 * geometry validation remains the writer's responsibility.
 */
typedef struct {
    int fd;
    char *path;
    bool writable;
    bool is_block;
    uint64_t size_bytes;
    dev_t device_number;
    dev_t host_device;
    ino_t inode;
} LdDevice;

/* True when this block node, an overlapping parent/child, or a storage mapping
 * that covers it is mounted in the current namespace.
 */
bool ld_device_number_is_mounted(dev_t device_number);
bool ld_path_is_mounted(const char *path);

/* Non-fatal opener. Returns 0 on success and -1 with errno on failure. */
int ld_device_try_open(const char *path, bool writable, LdDevice *device);

/* Query regular-file length or BLKGETSIZE64 capacity from an already-open fd. */
int ld_fd_size_bytes(int fd, uint64_t *size_bytes);

/* Render the stable transaction identity of an already-open target.
 * The caller owns the fixed buffer; no allocation or filesystem policy occurs.
 */
int ld_device_format_identity(const LdDevice *device,
                              char *buffer, size_t buffer_size);
int ld_fd_format_identity(int fd, char *buffer, size_t buffer_size);

bool ld_device_matches_identity(const LdDevice *device,
                                const char *expected_identity,
                                uint64_t expected_size);
bool ld_fd_matches_identity(int fd, const char *expected_identity,
                            uint64_t expected_size);

/* Open and enforce a transaction-recorded identity/capacity binding.
 * Returns an owned fd on success; the temporary LdDevice wrapper is consumed.
 */
int ld_device_open_verified_fd(const char *path, bool writable,
                               const char *expected_identity,
                               uint64_t expected_size);

/* Capture canonical path, stable object identity and capacity from one
 * validated read-only open. Returned strings are heap-owned by the caller.
 */
int ld_device_capture_binding(const char *path, char **canonical_path,
                              char **identity, uint64_t *size_bytes);

/* Fatal-policy convenience wrapper used where target-open failure aborts work. */
LdDevice ld_device_open(const char *path, bool writable);
void ld_device_close(LdDevice *device);

/* Performance hints only. They never relax correctness or safety policy. */
bool ld_device_is_rotational(const LdDevice *device);
bool ld_device_is_serial_flash(const LdDevice *device);

#endif
