// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_path.h"

#include "ld_runtime.h"

#include "infiltratr/arithmetic.h"
#include "infiltratr/posix.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *ld_path_append_suffix(const char *base, const char *suffix)
{
    const size_t base_length = strlen(base);
    const size_t suffix_length = strlen(suffix);
    size_t combined_length = 0U;
    size_t allocation_size = 0U;
    if (!infiltratr_size_add_checked(base_length, suffix_length,
                                     &combined_length) ||
        !infiltratr_size_add_checked(combined_length, 1U, &allocation_size))
        ld_die("path suffix length overflow");
    char *result = ld_xmalloc(allocation_size);
    if (!infiltratr_path_concat(result, allocation_size, base, suffix))
        ld_die("cannot append path suffix");
    return result;
}

bool ld_path_is_derived_from(const char *candidate, const char *base,
                             const char *suffix)
{
    if (candidate == NULL || base == NULL || suffix == NULL) return false;
    const size_t candidate_length = strlen(candidate);
    const size_t base_length = strlen(base);
    const size_t suffix_length = strlen(suffix);
    size_t expected_length = 0U;
    if (!infiltratr_size_add_checked(base_length, suffix_length,
                                     &expected_length) ||
        candidate_length != expected_length)
        return false;
    return memcmp(candidate, base, base_length) == 0 &&
           memcmp(candidate + base_length, suffix, suffix_length) == 0;
}

char *ld_path_parent_directory(const char *path)
{
    char *copy = ld_xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) {
        free(copy);
        return ld_xstrdup(".");
    }
    if (slash == copy) slash[1] = '\0';
    else *slash = '\0';
    return copy;
}

static int open_child_directory(int parent, const char *component)
{
#ifdef SYS_openat2
    /*
     * openat2 expresses the complete "stay beneath this trusted parent and do
     * not traverse any symlink/magic-link component" policy in one kernel
     * lookup. Older kernels fall back to the established openat/O_NOFOLLOW
     * path below; correctness does not depend on openat2 being available.
     */
    const struct open_how how = {
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
                   RESOLVE_NO_MAGICLINKS,
    };
    const long opened =
        syscall(SYS_openat2, parent, component, &how, sizeof(how));
    if (opened >= 0) return (int)opened;
    if (errno != ENOSYS && errno != EINVAL && errno != E2BIG)
        return -1;
#endif
    return openat(parent, component,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

static bool trusted_directory(const struct stat *status, uid_t effective_uid)
{
    if (!S_ISDIR(status->st_mode)) return false;
    const mode_t shared_write = status->st_mode & (S_IWGRP | S_IWOTH);
    if (effective_uid == 0)
        return status->st_uid == 0 && shared_write == 0;
    if (status->st_uid == effective_uid)
        return shared_write == 0;
    if (status->st_uid == 0) {
        if (shared_write == 0) return true;
        return (status->st_mode & S_ISVTX) != 0;
    }
    return false;
}

int ld_path_ensure_trusted_directory_tree(const char *path)
{
    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -1;
    char *copy = ld_xstrdup(path);
    char *save = NULL;
    const uid_t effective_uid = geteuid();
    int result = 0;

    for (char *component = strtok_r(copy, "/", &save);
         component != NULL;
         component = strtok_r(NULL, "/", &save)) {
        if (strcmp(component, ".") == 0 || component[0] == '\0') continue;
        if (strcmp(component, "..") == 0) {
            errno = EINVAL;
            result = -1;
            break;
        }

        int next = open_child_directory(directory, component);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(directory, component, 0755) != 0 && errno != EEXIST) {
                result = -1;
                break;
            }
            next = open_child_directory(directory, component);
        }
        if (next < 0) {
            result = -1;
            break;
        }

        struct stat status;
        if (fstat(next, &status) != 0) {
            const int failure = errno;
            (void)close(next);
            errno = failure;
            result = -1;
            break;
        }
        if (!trusted_directory(&status, effective_uid)) {
            (void)close(next);
            errno = EPERM;
            result = -1;
            break;
        }
        (void)close(directory);
        directory = next;
    }

    const int failure = errno;
    free(copy);
    (void)close(directory);
    if (result != 0) errno = failure;
    return result;
}


