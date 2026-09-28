// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_PATH_H
#define LD_PATH_H

#include <stdbool.h>
#include <stdint.h>

/* Returned strings are heap-owned by the caller. */
char *ld_path_append_suffix(const char *base, const char *suffix);

/* Exact lexical recovery binding: candidate must be byte-for-byte base+suffix.
 * This deliberately does not canonicalise either path.
 */
bool ld_path_is_derived_from(const char *candidate, const char *base,
                             const char *suffix);

char *ld_path_parent_directory(const char *path);

/*
 * Ensure every component of an absolute recovery-state directory is reached
 * without following symlinks and has ownership/write permissions acceptable to
 * the invoking effective uid. ".." components are rejected. Returns 0 on
 * success or -1 with errno.
 */
int ld_path_ensure_trusted_directory_tree(const char *path);

/*
 * Verify that the filesystem containing path's parent has enough immediately
 * available space to stage payload_bytes plus a 5% safety margin. This is a
 * filesystem-neutral transaction preflight, not a filesystem placement rule.
 * On ENOSPC, available_bytes and required_bytes are still returned.
 */
int ld_path_require_staging_capacity(const char *path, uint64_t payload_bytes,
                                     uint64_t *available_bytes,
                                     uint64_t *required_bytes);

#endif
