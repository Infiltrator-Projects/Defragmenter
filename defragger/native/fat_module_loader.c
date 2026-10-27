// SPDX-License-Identifier: GPL-3.0-or-later
/* Child-process host for the FAT engine exported by Filesystems' FAT*.so. */
#include "fat_defrag_abi.h"
#include "version.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef IFS_FAT_MODULE_PARENT
#define IFS_FAT_MODULE_PARENT "/System"
#endif
#define IFS_FAT_MODULE_DIR "Filesystems"

static int trusted_directory(int fd) {
    struct stat st;
    uid_t owner = 0;
#ifdef IFS_FAT_TESTING
    owner = geteuid();
#endif
    return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == owner &&
           (st.st_mode & 0022) == 0;
}

static int trusted_module(int fd) {
    struct stat st;
    uid_t owner = 0;
#ifdef IFS_FAT_TESTING
    owner = geteuid();
#endif
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == owner &&
           (st.st_mode & 0022) == 0;
}

#ifdef IFS_FAT_TESTING
static int test_atomic_file_write(const char *path, InfiltratrAtomicFileMode mode,
                                  InfiltratrAtomicFileWriter writer,
                                  const void *data) {
    int status = infiltratr_atomic_file_write(path, mode, writer, data);
    const char *value = getenv("IFS_TEST_CRASH_AFTER_JOURNAL");
    static unsigned long writes;
    if (status == 0 && value != NULL && *value != '\0' &&
        ++writes == strtoul(value, NULL, 10)) _exit(86);
    return status;
}
#endif

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("defragmenter-fat-worker %s\n", LD_VERSION);
        return 0;
    }
    if (argc < 3) {
        fputs("Usage: defragmenter-fat-worker COMMAND DEVICE [OPTIONS]\n", stderr);
        return 1;
    }

    /* The engine registers an atexit callback which still uses this table. */
    static IfsFatDefragHostV1 host = {
        .abi_version = IFS_FAT_DEFRAG_ABI_VERSION,
        .structure_bytes = sizeof(IfsFatDefragHostV1),
        .program_version = LD_VERSION,
#define IFS_FAT_BIND_HOST(name) .name = name,
        IFS_FAT_DEFRAG_HOST_FUNCTIONS(IFS_FAT_BIND_HOST)
#undef IFS_FAT_BIND_HOST
    };
    host.out = stdout;
    host.err = stderr;
#ifdef IFS_FAT_TESTING
    host.infiltratr_atomic_file_write = test_atomic_file_write;
#endif

    const char *module_parent = IFS_FAT_MODULE_PARENT;
#ifdef IFS_FAT_TESTING
    const char *test_parent = getenv("IFS_TEST_MODULE_PARENT");
    if (test_parent != NULL && *test_parent != '\0') module_parent = test_parent;
#endif
    int parent = open(module_parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0 || !trusted_directory(parent)) {
        fputs("FAT module parent is missing or not trusted\n", stderr);
        if (parent >= 0) close(parent);
        return 1;
    }
    int directory = openat(parent, IFS_FAT_MODULE_DIR,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (directory < 0 || !trusted_directory(directory)) {
        fputs("FAT module directory is missing or not trusted\n", stderr);
        if (directory >= 0) close(directory);
        return 1;
    }

    const char *names[] = {"FAT12.so", "FAT16.so", "FAT32.so"};
    const unsigned widths[] = {12, 16, 32};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        int fd = openat(directory, names[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT) continue;
        if (fd < 0 || !trusted_module(fd)) {
            fprintf(stderr, "FAT module is not trusted: %s\n", names[i]);
            if (fd >= 0) close(fd);
            close(directory);
            return 1;
        }
        char fd_path[64];
        if (snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd) >= (int)sizeof(fd_path)) {
            close(fd);
            close(directory);
            return 1;
        }
        void *module = dlopen(fd_path, RTLD_NOW | RTLD_LOCAL);
        if (module == NULL) {
            fprintf(stderr, "Cannot load %s: %s\n", names[i], dlerror());
            close(fd);
            close(directory);
            return 1;
        }
        void *symbol = dlsym(module, "infiltrator_filesystem_defrag_v1");
        if (symbol == NULL) {
            /* An older installed module must not prevent another FAT width
             * from working. A target needing it will fail below. */
            dlclose(module);
            close(fd);
            continue;
        }
        const IfsFatDefragV1 *(*entry)(void) = NULL;
        _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym pointer size");
        memcpy(&entry, &symbol, sizeof(entry));
        const IfsFatDefragV1 *api = entry != NULL ? entry() : NULL;
        if (api == NULL || api->abi_version != IFS_FAT_DEFRAG_ABI_VERSION ||
            api->structure_bytes < sizeof(*api) || api->fat_bits != widths[i] ||
            api->probe == NULL || api->run == NULL) {
            fprintf(stderr, "%s does not provide the required FAT defrag ABI\n", names[i]);
            dlclose(module);
            close(fd);
            close(directory);
            return 1;
        }
        int matched = api->probe(&host, argv[2]);
        if (matched < 0) {
            fprintf(stderr, "%s rejected its host ABI\n", names[i]);
            dlclose(module);
            close(fd);
            close(directory);
            return 1;
        }
        if (matched == 1) {
            close(directory);
            /* Deliberately keep the module loaded through process exit: the
             * engine registers a cleanup handler and owns signal state. */
            int result = api->run(&host, argc, argv);
            return result < 0 ? 1 : result;
        }
        dlclose(module);
        close(fd);
    }
    close(directory);
    fputs("No installed FAT defrag module matches the target; install or update the matching Filesystems module\n", stderr);
    return 1;
}
