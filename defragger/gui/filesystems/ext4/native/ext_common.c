// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_native.h"

#include "ld_runtime.h"

#include "infiltratr/arithmetic.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <unistd.h>

#define EXT3_INCOMPAT_MASK UINT32_C(0x001f)
#define EXT3_RO_COMPAT_MASK UINT32_C(0x0003)

void ext_set_error(char **error, const char *format, ...) {
    if (error == NULL || *error != NULL) return;
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int required = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (required < 0) {
        va_end(arguments);
        *error = ld_xstrdup("EXT engine error");
        return;
    }
    *error = ld_xmalloc((size_t)required + 1U);
    (void)vsnprintf(*error, (size_t)required + 1U, format, arguments);
    va_end(arguments);
}

static int compare_range(const void *left, const void *right) {
    const ExtRange *a = left;
    const ExtRange *b = right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

void ext_range_push(ExtRangeVec *vec, uint64_t start, uint64_t end) {
    if (end <= start) return;
    if (vec->count == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&vec->items, &vec->capacity,
                                  sizeof(*vec->items), vec->count + 1U, 32U))
        ld_die("cannot grow EXT range vector");
    vec->items[vec->count++] = (ExtRange){.start = start, .end = end};
}

void ext_range_free(ExtRangeVec *vec) {
    if (vec == NULL) return;
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

void ext_range_sort_merge(ExtRangeVec *vec) {
    if (vec == NULL || vec->count < 2) return;
    qsort(vec->items, vec->count, sizeof(*vec->items), compare_range);
    size_t write_index = 0;
    for (size_t index = 1; index < vec->count; ++index) {
        ExtRange *current = &vec->items[write_index];
        ExtRange next = vec->items[index];
        if (next.start <= current->end) {
            if (next.end > current->end) current->end = next.end;
        } else {
            vec->items[++write_index] = next;
        }
    }
    vec->count = write_index + 1U;
}

static int physical_size(const char *path, uint64_t *size, char **error) {
    struct stat status;
    if (stat(path, &status) != 0) {
        ext_set_error(error, "cannot stat EXT target %s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISREG(status.st_mode)) {
        *size = (uint64_t)status.st_size;
        return 0;
    }
    if (!S_ISBLK(status.st_mode)) {
        ext_set_error(error, "EXT target must be a block device or regular filesystem image");
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ext_set_error(error, "cannot open EXT target %s: %s", path, strerror(errno));
        return -1;
    }
    uint64_t found = 0;
    int result = ioctl(fd, BLKGETSIZE64, &found);
    int saved = errno;
    (void)close(fd);
    if (result != 0) {
        ext_set_error(error, "cannot read EXT target size: %s", strerror(saved));
        return -1;
    }
    *size = found;
    return 0;
}

int ext_read_geometry(const char *path, ExtGeometry *geometry, char **error) {
    if (geometry == NULL) return -1;
    memset(geometry, 0, sizeof(*geometry));
    uint64_t physical_bytes = 0;
    if (physical_size(path, &physical_bytes, error) != 0) return -1;

    ExtFs *fs = NULL;
    if (ext_fs_open(path, false, &fs, error) != 0) return -1;
    uint32_t block_size = ext_fs_block_size(fs);
    uint64_t blocks = ext_fs_blocks_count(fs);
    uint64_t free_blocks = ext_fs_free_blocks(fs);
    uint64_t physical_blocks = physical_bytes / block_size;
    if (blocks == 0U || free_blocks > blocks || blocks > physical_blocks) {
        ext_fs_close(fs);
        ext_set_error(error, "EXT filesystem geometry exceeds the target device");
        return -1;
    }

    geometry->block_size = block_size;
    geometry->total_blocks = blocks;
    geometry->free_blocks = free_blocks;
    geometry->physical_blocks = physical_blocks;
    geometry->physical_bytes = physical_bytes;
    geometry->first_data_block = ext_fs_first_data_block(fs);
    geometry->ro_compat = ext_fs_ro_compat(fs);
    geometry->incompat = ext_fs_incompat(fs);
    geometry->compat = ext_fs_compat(fs);
    memcpy(geometry->uuid, ext_fs_uuid(fs), sizeof(geometry->uuid));

    if ((geometry->incompat & ~EXT3_INCOMPAT_MASK) != 0U ||
        (geometry->ro_compat & ~EXT3_RO_COMPAT_MASK) != 0U) {
        memcpy(geometry->filesystem, "ext4", 5U);
    } else if ((geometry->compat & EXT_FEATURE_COMPAT_HAS_JOURNAL) != 0U) {
        memcpy(geometry->filesystem, "ext3", 5U);
    } else {
        memcpy(geometry->filesystem, "ext2", 5U);
    }
    ext_fs_close(fs);
    return 0;
}

int ext_open_fs(const char *path, bool writable, ExtFs **fs, char **error) {
    return ext_fs_open(path, writable, fs, error);
}

int ext_open_fs_under_lock(const char *path, bool writable, ExtFs **fs,
                           char **error) {
    /*
     * The caller already owns the Defragmenter descriptor lock.  Unlike the
     * former libext2fs wrapper there is no second path-based O_EXCL authority.
     */
    return ext_fs_open(path, writable, fs, error);
}

int ext_validate_metadata(ExtFs *fs, bool verify_inodes, char **error) {
    return ext_fs_validate_metadata(fs, verify_inodes, error);
}

int ext_validate_writer_support(ExtFs *fs, const ExtGeometry *geometry,
                                char **error) {
    if ((geometry->ro_compat & EXT_FEATURE_RO_COMPAT_BIGALLOC) != 0U) {
        ext_set_error(error,
            "EXT bigalloc filesystems are outside the bounded native writer");
        return -1;
    }
    if ((geometry->incompat & EXT_FEATURE_INCOMPAT_META_BG) != 0U) {
        ext_set_error(error,
            "EXT meta_bg analysis is supported, but raw relayout remains fail-closed until descriptor-backup mutation is qualified");
        return -1;
    }
    if ((geometry->incompat &
         (EXT_FEATURE_INCOMPAT_COMPRESSION |
          EXT_FEATURE_INCOMPAT_RECOVER |
          EXT_FEATURE_INCOMPAT_JOURNAL_DEV |
          EXT_FEATURE_INCOMPAT_DIRDATA)) != 0U) {
        ext_set_error(error,
            "EXT filesystem has an incompatible state outside the bounded raw writer");
        return -1;
    }
    if ((geometry->ro_compat &
         (EXT_FEATURE_RO_COMPAT_HAS_SNAPSHOT |
          EXT_FEATURE_RO_COMPAT_READONLY |
          EXT_FEATURE_RO_COMPAT_SHARED_BLOCKS |
          EXT_FEATURE_RO_COMPAT_ORPHAN_PRESENT)) != 0U) {
        ext_set_error(error,
            "EXT filesystem has read-only/snapshot/shared/orphan state outside the bounded raw writer");
        return -1;
    }
    if ((ext_fs_state(fs) & EXT_VALID_FS) == 0U) {
        ext_set_error(error,
            "EXT filesystem is not marked clean; refusing raw mutation");
        return -1;
    }
    if (geometry->physical_blocks > geometry->total_blocks + 255U) {
        ext_set_error(error,
            "EXT active filesystem does not span the target; Defragmenter will not resize it implicitly");
        return -1;
    }
    return ext_validate_metadata(fs, true, error);
}

