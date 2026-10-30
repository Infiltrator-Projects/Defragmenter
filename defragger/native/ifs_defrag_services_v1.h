/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Filesystem-neutral safety, I/O, Stop, journal and recovery host services. */
#ifndef IFS_DEFRAG_SERVICES_V1_H
#define IFS_DEFRAG_SERVICES_V1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "ld_device.h"
#define IFS_DEFRAG_SERVICES_V1 1u
typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    ssize_t(*ld_pread_full)(int fd, void *p, size_t n, uint64_t at);
    ssize_t(*ld_pwrite_full)(int fd, const void *p, size_t n, uint64_t at);
    bool(*ld_bitmap_size)(uint64_t bits, size_t *bytes);
    uint8_t *(*ld_bitmap_calloc)(uint64_t bits);
    bool(*ld_bitmap_get)(const uint8_t *p, uint64_t bit);
    void(*ld_bitmap_set)(uint8_t *p, uint64_t bit, bool value);
    int(*ld_sync_fd)(int fd);
    int(*ld_fd_size_bytes)(int fd, uint64_t *bytes);
    int(*ld_device_capture_binding)(const char *path, char **canonical, char **identity, uint64_t *bytes);
    int(*ld_device_open_verified_fd)(const char *path, bool writable, const char *identity, uint64_t bytes);
    bool(*ld_path_is_mounted)(const char *path);
    char *(*ld_path_parent_directory)(const char *path);
    char *(*ld_path_append_suffix)(const char *base, const char *suffix);
    bool(*ld_path_is_derived_from)(const char *candidate, const char *base, const char *suffix);
    int(*ld_path_ensure_trusted_directory_tree)(const char *path);
    int(*ld_path_require_staging_capacity)(const char *path, uint64_t n, uint64_t *available, uint64_t *required);
    char *(*ld_xstrdup)(const char *s);
    int(*ld_emit_result_event)(FILE *stream, const char *operation, const char *status, const char *message);
    bool(*ld_stop_requested)(void);
    void(*ld_stop_clear)(void);
    void(*ld_stop_install_handlers)(void);
    void(*ld_stop_report_ready)(void);
    /* NTFS workers require these additional filesystem-neutral safety hooks.
     * Appended fields preserve the existing prefix; structure_bytes guards
     * against loading a host with an older incomplete table. */
    void (*ld_die)(const char *message);
    char *(*ld_xstrndup)(const char *, size_t);
    bool (*ld_device_number_is_mounted)(dev_t);
    bool (*ld_fd_matches_identity)(int, const char *, uint64_t);
    LdDevice (*ld_device_open)(const char *, bool);
    void (*ld_device_close)(LdDevice *);
} IfsDefragServicesV1;
#endif
