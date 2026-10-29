/* SPDX-License-Identifier: GPL-3.0-or-later
 * Generic child-process module host. All target identity, overlap, descriptor
 * I/O, private durable files, cancellation and crypto remain mechanisms. The
 * loaded filesystem owns interpretation, allocation, placement and recovery.
 */
#include "filesystem_module_host.h"
#include "filesystem_defrag_abi.h"
#include "version.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_stop.h"
#include "infiltratr/posix.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { LdDevice device; bool stage; } Target;
typedef struct {
#ifdef IFS_MODULE_TESTING
    unsigned long journals, writes;
    int stopped;
#else
    unsigned unused;
#endif
} Context;
static uid_t owner(void) {
#ifdef IFS_MODULE_TESTING
    return geteuid();
#else
    return 0;
#endif
}
static int trusted(int fd, int directory) {
    struct stat st;
    return fstat(fd, &st) == 0 && (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)) &&
           st.st_uid == owner() && (st.st_mode & 0022) == 0;
}
static int parents(const char *path) {
    char *parent = ld_path_parent_directory(path);
    if (!parent) return -1;
    int r = ld_path_ensure_trusted_directory_tree(parent); free(parent); return r;
}
static int wrap_target(Target *t, IfsDefragTarget *out) {
    memset(out, 0, sizeof(*out));
    if (snprintf(out->path, sizeof(out->path), "%s", t->device.path) >= (int)sizeof(out->path) ||
        ld_device_format_identity(&t->device, out->identity, sizeof(out->identity)) != 0) return -1;
    out->bytes = t->device.size_bytes; out->handle = t; return 0;
}
static int target_open(void *context, const char *path, int write,
                        const char *identity, uint64_t bytes, IfsDefragTarget *out) {
    (void)context;
    Target *t = calloc(1, sizeof(*t)); if (!t) return -1;
    t->device.fd = -1;
    if (ld_device_try_open(path, write != 0, &t->device) != 0) { free(t); return -1; }
    if ((identity && !ld_device_matches_identity(&t->device, identity, bytes)) ||
        (bytes && bytes != t->device.size_bytes) ||
        flock(t->device.fd, (write ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0 || wrap_target(t, out)) {
        ld_device_close(&t->device); free(t); return -1;
    }
    return 0;
}
static int stage_create(void *context, const char *path, uint64_t bytes, IfsDefragTarget *out) {
    (void)context;
    uint64_t available = 0, required = 0;
    if (bytes > INT64_MAX || parents(path) ||
        ld_path_require_staging_capacity(path, bytes, &available, &required)) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int r = ftruncate(fd, (off_t)bytes);
    if (!r) r = fsync(fd);
    close(fd);
    if (r || target_open(context, path, 1, NULL, bytes, out)) {
        (void)infiltratr_unlink_durable(path, true); return -1;
    }
    ((Target *)out->handle)->stage = true; return 0;
}
static void target_close(void *context, IfsDefragTarget *target) {
    (void)context;
    Target *t = target->handle;
    if (t) { ld_device_close(&t->device); free(t); }
    memset(target, 0, sizeof(*target));
}
static int target_read(void *context, const IfsDefragTarget *target,
                       uint64_t at, void *out, uint32_t bytes) {
    (void)context; Target *t = target->handle;
    if (!t || at > target->bytes || bytes > target->bytes - at) return -1;
    return ld_pread_full(t->device.fd, out, bytes, at) == (ssize_t)bytes ? 0 : -1;
}
#ifdef IFS_MODULE_TESTING
static int test_count(const char *name, unsigned long count) {
    const char *s = getenv(name); char *end = NULL;
    if (!s || !*s) return 0;
    errno = 0; unsigned long n = strtoul(s, &end, 10);
    return !errno && end && !*end && n == count;
}
#endif
static int target_write(void *context, const IfsDefragTarget *target,
                        uint64_t at, const void *data, uint32_t bytes) {
    Target *t = target->handle;
    if (!t || !t->device.writable || at > target->bytes || bytes > target->bytes - at ||
        !ld_device_matches_identity(&t->device, target->identity, target->bytes)) return -1;
    int r = ld_pwrite_full(t->device.fd, data, bytes, at) == (ssize_t)bytes ? 0 : -1;
#ifdef IFS_MODULE_TESTING
    Context *c = context;
    if (!r && !t->stage) {
        ++c->writes;
        if (test_count("IFS_TEST_CRASH_AFTER_DEVICE_WRITE", c->writes)) _exit(87);
        if (test_count("IFS_TEST_STOP_AFTER_DEVICE_WRITE", c->writes)) c->stopped = 1;
    }
#else
    (void)context;
#endif
    return r;
}
static int target_sync(void *context, const IfsDefragTarget *target) {
    (void)context; Target *t = target->handle;
    return t && ld_sync_fd(t->device.fd) == 0 ? 0 : -1;
}
static int file_exists(void *context, const char *path) {
    (void)context; struct stat st;
    if (!lstat(path, &st)) return 1;
    return errno == ENOENT ? 0 : -1;
}
static int file_read(void *context, const char *path, void *out, uint32_t cap, uint32_t *got) {
    (void)context; *got = 0;
    if (parents(path)) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return -1;
    struct stat st; int r = -1;
    if (trusted(fd, 0) && !fstat(fd, &st) && st.st_size >= 0 && (uint64_t)st.st_size <= cap &&
        ld_pread_full(fd, out, (size_t)st.st_size, 0) == st.st_size) {
        *got = (uint32_t)st.st_size; r = 0;
    }
    close(fd); return r;
}
typedef struct { const void *bytes; uint32_t count; } Payload;
static bool write_payload(FILE *file, const void *opaque) {
    const Payload *p = opaque;
    return fwrite(p->bytes, 1, p->count, file) == p->count && !ferror(file);
}
static int file_atomic(void *context, const char *path, const void *data, uint32_t bytes) {
    if (parents(path)) return -1;
    Payload p = {data, bytes};
    int r = infiltratr_atomic_file_write(path, INFILTRATR_ATOMIC_FILE_PRIVATE, write_payload, &p);
#ifdef IFS_MODULE_TESTING
    Context *c = context;
    if (!r && test_count("IFS_TEST_CRASH_AFTER_JOURNAL", ++c->journals)) _exit(86);
#else
    (void)context;
#endif
    return r ? -1 : 0;
}
static int file_remove(void *context, const char *path) {
    (void)context; return infiltratr_unlink_durable(path, true) ? -1 : 0;
}
static void *digest_create(void *context) {
    (void)context; EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (c && EVP_DigestInit_ex(c, EVP_sha256(), NULL) == 1) return c;
    EVP_MD_CTX_free(c); return NULL;
}
static int digest_update(void *context, void *digest, const void *data, size_t bytes) {
    (void)context; return EVP_DigestUpdate(digest, data, bytes) == 1 ? 0 : -1;
}
static int digest_finish(void *context, void *digest, uint8_t out[32]) {
    (void)context; unsigned n = 0;
    return EVP_DigestFinal_ex(digest, out, &n) == 1 && n == 32u ? 0 : -1;
}
static void digest_release(void *context, void *digest) { (void)context; EVP_MD_CTX_free(digest); }
static int stop_requested(void *context) {
#ifdef IFS_MODULE_TESTING
    if (((Context *)context)->stopped) return 1;
#else
    (void)context;
#endif
    return ld_stop_requested() ? 1 : 0;
}
static int print(void *context, int channel, const char *format, ...) {
    (void)context; FILE *file = channel ? stderr : stdout; va_list ap;
    va_start(ap, format); int r = vfprintf(file, format, ap); va_end(ap); fflush(file); return r;
}
int ifs_filesystem_journal_matches(const char *path, const char *format) {
    if (!path || !format) return 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return 0;
    char first[64] = {0}, expected[64];
    ssize_t n = read(fd, first, sizeof(first)-1u); close(fd);
    int bytes = snprintf(expected, sizeof(expected), "IFS-DEFRAG-JOURNAL:1:%s\n", format);
    return n >= bytes && bytes > 0 && bytes < (int)sizeof(expected) && !memcmp(first, expected, (size_t)bytes);
}
int ifs_filesystem_module_call(const char *format, const char *filename, const char *interface,
                               IfsFilesystemCall call, void *user) {
    const char *parent = "/System";
#ifdef IFS_MODULE_TESTING
    const char *override = getenv("IFS_TEST_MODULE_PARENT");
    if (override && *override) parent = override;
#endif
    if (!format || !filename || !*filename || !interface || !call || strchr(filename, '/') || strlen(format) > 15u) return 1;
    int p = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (p < 0 || !trusted(p, 1)) { if (p >= 0) close(p); goto missing; }
    int d = openat(p, "Filesystems", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); close(p);
    if (d < 0 || !trusted(d, 1)) { if (d >= 0) close(d); goto missing; }
    int fd = openat(d, filename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); close(d);
    if (fd < 0 || !trusted(fd, 0)) { if (fd >= 0) close(fd); goto missing; }
    char path[64]; (void)snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    void *image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!image) { fprintf(stderr, "Cannot load %s: %s\n", filename, dlerror()); close(fd); return 1; }
    void *symbol = dlsym(image, "infiltrator_filesystem_defrag_v2");
    const IfsDefragModuleV2 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsDefragModuleV2 *api = entry ? entry() : NULL;
    if (!api || api->abi_version != IFS_DEFRAG_ABI_V2 || api->structure_bytes < sizeof(*api) ||
        !api->format_id || strcmp(api->format_id, format) || !api->run ||
        (api->capabilities & 15u) != 15u) {
        fprintf(stderr, "%s lacks the required %s Defragmenter interface\n", filename, format);
        dlclose(image); close(fd); return 1;
    }
    Context context = {0};
    IfsDefragHostV2 host = {IFS_DEFRAG_ABI_V2, sizeof(host), &context, LD_VERSION,
        malloc, free, target_open, stage_create, target_close, target_read, target_write,
        target_sync, file_exists, file_read, file_atomic, file_remove,
        digest_create, digest_update, digest_finish, digest_release, stop_requested, print, snprintf};
    void *requested = dlsym(image, interface);
    int r = requested ? call(&host, requested, user) : 1;
    dlclose(image); close(fd); return r;
missing:
    fprintf(stderr, "Install or update the trusted /System/Filesystems/%s module; %s operations require its SO interface.\n", filename, format);
    return 1;
}
typedef struct { int argc; char **argv; } Command;
static int run_command(const IfsDefragHostV2 *host, void *symbol, void *user) {
    const IfsDefragModuleV2 *(*entry)(void) = NULL;
    memcpy(&entry, &symbol, sizeof(entry));
    Command *command = user;
    return entry()->run(host, command->argc, command->argv);
}
int ifs_filesystem_module_run(const char *format, const char *filename, int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "defrag") || !strcmp(argv[1], "growth-defrag") || !strcmp(argv[1], "recover"))) {
        ld_stop_clear(); ld_stop_install_handlers(); ld_stop_report_ready();
    }
    Command command = {argc, argv};
    return ifs_filesystem_module_call(format, filename, "infiltrator_filesystem_defrag_v2", run_command, &command);
}
