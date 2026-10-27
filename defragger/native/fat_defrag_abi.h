// SPDX-License-Identifier: GPL-3.0-or-later
/* Linux process-hosted FAT defragmentation interface. The host supplies the
 * system and generic safety services; filesystem algorithms live in the .so.
 * One invocation per child process; do not dlclose while it is running. */
#ifndef IFS_FAT_DEFRAG_ABI_H
#define IFS_FAT_DEFRAG_ABI_H
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_protocol.h"
#include "ld_runtime.h"
#include "ld_stop.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/config.h"
#include "infiltratr/core.h"
#include "infiltratr/posix.h"
#include "infiltratr/posix_path.h"
#include "infiltratr/quantity.h"
#include "infiltratr/token.h"

#include <stdint.h>
#define IFS_FAT_DEFRAG_ABI_VERSION 1u
#define IFS_FAT_DEFRAG_HOST_FUNCTIONS(X) \
    X(__errno_location) \
    X(access) \
    X(atexit) \
    X(close) \
    X(exit) \
    X(fclose) \
    X(ferror) \
    X(fflush) \
    X(flock) \
    X(fopen) \
    X(fprintf) \
    X(fputs) \
    X(free) \
    X(fsync) \
    X(getline) \
    X(infiltratr_array_reserve) \
    X(infiltratr_atomic_file_write) \
    X(infiltratr_config_parse_line) \
    X(infiltratr_monotonic_seconds) \
    X(infiltratr_parse_binary_quantity_u64) \
    X(infiltratr_parse_u64) \
    X(infiltratr_parse_u64_range) \
    X(infiltratr_parse_u64_token) \
    X(infiltratr_path_join) \
    X(infiltratr_size_add_checked) \
    X(infiltratr_size_multiply_checked) \
    X(infiltratr_trim_line_end) \
    X(infiltratr_u64_counter_rate) \
    X(infiltratr_unlink_durable) \
    X(ld_bitmap_calloc) \
    X(ld_bitmap_get) \
    X(ld_bitmap_set) \
    X(ld_bitmap_size) \
    X(ld_default_ram_limit) \
    X(ld_default_worker_count) \
    X(ld_device_close) \
    X(ld_device_format_identity) \
    X(ld_device_is_rotational) \
    X(ld_device_is_serial_flash) \
    X(ld_device_matches_identity) \
    X(ld_device_open) \
    X(ld_die) \
    X(ld_die_errno) \
    X(ld_emit_result_event) \
    X(ld_online_cpu_count) \
    X(ld_path_ensure_trusted_directory_tree) \
    X(ld_path_parent_directory) \
    X(ld_pread_full) \
    X(ld_pwrite_full) \
    X(ld_runtime_set_program_name) \
    X(ld_stop_clear) \
    X(ld_stop_install_handlers) \
    X(ld_stop_report_ready) \
    X(ld_stop_requested) \
    X(ld_xcalloc) \
    X(ld_xmalloc) \
    X(ld_xstrdup) \
    X(localtime_r) \
    X(open) \
    X(posix_memalign) \
    X(printf) \
    X(pthread_create) \
    X(pthread_join) \
    X(pthread_mutex_destroy) \
    X(pthread_mutex_init) \
    X(pthread_mutex_lock) \
    X(pthread_mutex_unlock) \
    X(putchar) \
    X(qsort) \
    X(setvbuf) \
    X(snprintf) \
    X(strcmp) \
    X(strftime) \
    X(time) \
    X(vfprintf)

typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    const char *program_version;
    FILE *out;
    FILE *err;
#define IFS_FAT_DECLARE_HOST(name) __typeof__(name) *name;
    IFS_FAT_DEFRAG_HOST_FUNCTIONS(IFS_FAT_DECLARE_HOST)
#undef IFS_FAT_DECLARE_HOST
} IfsFatDefragHostV1;

typedef struct {
    uint32_t abi_version;
    uint32_t structure_bytes;
    uint32_t fat_bits;
    /* Return 1 only for a matching geometry, 0 for no match, -1 for bad host. */
    int (*probe)(const IfsFatDefragHostV1 *, const char *path);
    int (*run)(const IfsFatDefragHostV1 *, int argc, char **argv);
} IfsFatDefragV1;
const IfsFatDefragV1 *infiltrator_filesystem_defrag_v1(void);
#endif
