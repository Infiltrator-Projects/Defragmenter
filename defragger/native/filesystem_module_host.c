/* SPDX-License-Identifier: GPL-3.0-or-later
 * Generic child-process module host. All target identity, overlap, descriptor
 * I/O, private durable files, cancellation and crypto remain mechanisms. The
 * loaded filesystem owns interpretation, allocation, placement and recovery.
 */
#include "filesystem_module_host.h"
#include "filesystem_defrag_abi.h"
#include "infiltratr/filesystem_module_v2.h"
#include "ld_generic_layout.h"
#include "version.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_stop.h"
#include "infiltratr/posix.h"
#include <ctype.h>
#include <dirent.h>
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
#include <sys/ioctl.h>
#include <sys/random.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The exclusive, preflight-qualified open pins the actual kernel object.
 * Per-block write callbacks check the open descriptor and this immutable
 * binding; costly /sys + udev identity discovery is bounded rather than
 * repeated for each 512-byte OFS block. Hardware identity is still refreshed
 * at least every 128 successful writes and before every durability barrier. */
enum { TARGET_IDENTITY_REFRESH_WRITES = 128 };
typedef struct {
    LdDevice device;
    bool stage;
    unsigned writes_since_identity_refresh;
    char bound_identity[IFS_DEFRAG_ID_BYTES];
} Target;
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
    (void)memcpy(t->bound_identity, out->identity, sizeof(t->bound_identity));
    out->bytes = t->device.size_bytes; out->handle = t; return 0;
}
/* fstat() tests the already-exclusive open kernel object, not a pathname
 * that an automounter or udev could rename or replace between writes. */
static bool target_fd_matches_binding(const Target *t, const IfsDefragTarget *out) {
    if (!t || !out || t->device.fd < 0 ||
        out->bytes != t->device.size_bytes ||
        memcmp(out->identity, t->bound_identity, sizeof(t->bound_identity)) != 0)
        return false;
    struct stat opened;
    if (fstat(t->device.fd, &opened) != 0) return false;
    return ((opened.st_mode & S_IFMT) ==
            (t->device.is_block ? S_IFBLK : S_IFREG)) &&
           opened.st_dev == t->device.host_device &&
           opened.st_ino == t->device.inode &&
           (!t->device.is_block || opened.st_rdev == t->device.device_number);
}
static int target_open(void *context, const char *path, int write,
                        const char *identity, uint64_t bytes, IfsDefragTarget *out) {
    (void)context;
    Target *t = calloc(1, sizeof(*t)); if (!t) return -1;
    t->device.fd = -1;
    if (ld_device_try_open(path, write != 0, &t->device) != 0) { free(t); return -1; }
    if ((identity && !ld_device_matches_identity(&t->device, identity, bytes)) ||
        (bytes && bytes != t->device.size_bytes)) {
        ld_device_close(&t->device); free(t); errno = ESTALE; return -1;
    }
    if (flock(t->device.fd, (write ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        const int failure = errno;
        ld_device_close(&t->device); free(t); errno = failure; return -1;
    }
    if (wrap_target(t, out)) {
        ld_device_close(&t->device); free(t); errno = EINVAL; return -1;
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
    if (!t || !t->device.writable ||
        !target_fd_matches_binding(t, target) ||
        at > target->bytes || bytes > target->bytes - at) return -1;
    if (t->writes_since_identity_refresh >= TARGET_IDENTITY_REFRESH_WRITES) {
        if (!ld_device_matches_identity(&t->device, t->bound_identity,
                                        t->device.size_bytes))
            return -1;
        t->writes_since_identity_refresh = 0;
    }
    int r = ld_pwrite_full(t->device.fd, data, bytes, at) == (ssize_t)bytes ? 0 : -1;
    if (!r) ++t->writes_since_identity_refresh;
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
    (void)context; Target *t = target ? target->handle : NULL;
    if (!t || !target_fd_matches_binding(t, target) ||
        !ld_device_matches_identity(&t->device, t->bound_identity,
                                    t->device.size_bytes))
        return -1;
    if (ld_sync_fd(t->device.fd) != 0) return -1;
    t->writes_since_identity_refresh = 0;
    return 0;
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
    if (!format || !filename || !*filename || !interface || !call ||
        strchr(filename, '/') || !ifs_fs_format_id_valid(format)) return 1;
    /* A non-Common filesystem export cannot be selected, loaded or invoked,
     * even by an older worker that still passes its historical symbol name.
     * Keep this check before opening any target or calling dlopen(). */
    if (strcmp(interface, IFS_FS_COMMON_EXPORT_V2) != 0) {
        fputs("Refusing obsolete or private filesystem ABI: Common V2 only\n",
              stderr);
        return 2;
    }
    int p = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (p < 0 || !trusted(p, 1)) { if (p >= 0) close(p); goto missing; }
    int d = openat(p, "Filesystems", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); close(p);
    if (d < 0 || !trusted(d, 1)) { if (d >= 0) close(d); goto missing; }
    int fd = openat(d, filename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); close(d);
    if (fd < 0 || !trusted(fd, 0)) { if (fd >= 0) close(fd); goto missing; }
    char path[64]; (void)snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    void *image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!image) { fprintf(stderr, "Cannot load %s: %s\n", filename, dlerror()); close(fd); return 1; }
    /* The Common V2 callback validates its canonical descriptor before
     * obtaining a target lease. No additional independent V2 ABI exists. */
    Context context = {0};
    IfsDefragHostV2 host = {IFS_DEFRAG_ABI_V2, sizeof(host), &context, LD_VERSION,
        malloc, free, target_open, stage_create, target_close, target_read, target_write,
        target_sync, file_exists, file_read, file_atomic, file_remove,
        digest_create, digest_update, digest_finish, digest_release, stop_requested, print, snprintf,
        ld_generic_layout_plan, ld_generic_layout_plan_file_extents};
    void *requested = dlsym(image, interface);
    int r = requested ? call(&host, requested, user) : 1;
    dlclose(image); close(fd); return r;
missing:
    fprintf(stderr, "Install or update the trusted /System/Filesystems/%s module; %s operations require its SO interface.\n", filename, format);
    return 1;
}
/* Open the authority namespace once. Every path component must be owned
 * by root and not writable by group/others; symlink traversal is forbidden. */
static int ifs_open_module_directory(void) {
    const char *parent_path="/System";
#ifdef IFS_MODULE_TESTING
    const char *test_parent=getenv("IFS_TEST_MODULE_PARENT");
    if(test_parent && *test_parent) parent_path=test_parent;
#endif
    int parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0 || !trusted(parent, 1)) {
        if (parent >= 0) close(parent);
        return -1;
    }
    int directory = openat(parent, "Filesystems",
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (directory < 0 || !trusted(directory, 1)) {
        if (directory >= 0) close(directory);
        return -1;
    }
    return directory;
}

static int ifs_safe_module_filename(const char *name) {
    const size_t length = strlen(name);
    if (length < 4u || length > 59u ||
        strcmp(name + length - 3u, ".so") != 0)
        return 0;
    for (size_t n = 0u; n < length; ++n) {
        const unsigned char ch = (unsigned char)name[n];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-' || ch == '.'))
            return 0;
    }
    return 1;
}

/* Inspect metadata without opening or writing any filesystem target. Never
 * trust a filename as a format identity: that identity comes from the SO's
 * versioned interface. An incompatible module is not a generic provider. */
static int ifs_probe_module_v2(int directory, const char *name,
                               char format[IFS_FS_MAX_FORMAT_ID+1u], uint64_t *capabilities,
                               uint64_t *group_mask, int require_defrag) {
    if (!ifs_safe_module_filename(name) || !format || !capabilities || !group_mask)
        return -1;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (!trusted(fd, 0)) { close(fd); return -1; }
    char path[64];
    if (snprintf(path, sizeof(path), "/proc/self/fd/%d", fd) >=
        (int)sizeof(path)) {
        close(fd);
        return -1;
    }
    void *image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!image) { close(fd); return -1; }
    void *symbol = dlsym(image, IFS_FS_COMMON_EXPORT_V2);
    IfsFsModuleApiEntryV2 entry = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsFsModuleApiV2 *api = entry ? entry() : NULL;
    int result = -1;
    if (api && ifs_fs_api_v2_valid(api, api->format_id) &&
        (api->runtime_profiles & IFS_FS_PROFILE_HOSTED)) {
        const IfsFsDefragOpsV2 *defrag =
            (const IfsFsDefragOpsV2 *)ifs_fs_group_v2(
                api, IFS_FS_GROUP_DEFRAG);
        if (!require_defrag || (defrag && defrag->header.capabilities != 0u)) {
            const size_t length = strnlen(api->format_id, IFS_FS_MAX_FORMAT_ID+1u);
            if (length > 0u && length <= IFS_FS_MAX_FORMAT_ID) {
                memcpy(format, api->format_id, length + 1u);
                *capabilities = defrag ? defrag->header.capabilities : 0u;
                *group_mask = api->group_mask;
                result = 0;
            }
        }
    }
    dlclose(image);
    close(fd);
    return result;
}

/* Read-only inventory includes all trusted hosted Common V2 providers, even
 * CORE/VOLUME-only modules. The mutation lookup still requires DEFRAG. */
int ifs_filesystem_module_list_v2(void) {
    int directory = ifs_open_module_directory();
    if (directory < 0) {
        fputs("{\"schema\":1,\"authority_available\":false,\"modules\":[]}\n", stdout);
        return 0;
    }
    DIR *entries = fdopendir(directory);
    if (!entries) { close(directory); return 1; }
    int first = 1;
    fputs("{\"schema\":1,\"authority_available\":true,\"modules\":[", stdout);
    struct dirent *item;
    while ((item = readdir(entries)) != NULL) {
        char format[IFS_FS_MAX_FORMAT_ID+1u];
        uint64_t capabilities = 0u, groups = 0u;
        if (ifs_probe_module_v2(directory, item->d_name,
                                format, &capabilities, &groups, 0) != 0)
            continue;
        if (!first) fputc(',', stdout);
        first = 0;
        const int has_defrag =
            (groups & IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)) != 0u;
        printf("{\"format_id\":\"%s\",\"filename\":\"%s\","
               "\"abi\":2,\"group_mask\":%llu,\"capabilities\":%llu,"
               "\"defrag_group\":%s}",
               format, item->d_name, (unsigned long long)groups,
               (unsigned long long)capabilities,
               has_defrag ? "true" : "false");
    }
    fputs("]}\n", stdout);
    closedir(entries);
    return ferror(stdout) ? 1 : 0;
}

/* Resolve by the module's exported format_id, not a guessed name or a host
 * filesystem registry. Duplicate providers fail closed. The dispatcher then
 * loads the returned trusted SO again and revalidates its ABI and identity. */
static int ifs_filesystem_module_find_selected_v2(
    const char *format, char *filename, size_t capacity, int require_defrag,
    uint64_t *actual_capabilities) {
    if (!format || !filename || capacity == 0u) return -1;
    if(!ifs_fs_format_id_valid(format))return -1;
    int directory = ifs_open_module_directory();
    if (directory < 0) return -1;
    DIR *entries = fdopendir(directory);
    if (!entries) { close(directory); return -1; }
    char selected[64] = "";
    unsigned int matches = 0u;
    uint64_t selected_capabilities = 0u;
    struct dirent *item;
    while ((item = readdir(entries)) != NULL) {
        char discovered[IFS_FS_MAX_FORMAT_ID+1u];
        uint64_t capabilities = 0u, groups = 0u;
        if (ifs_probe_module_v2(directory, item->d_name,
                                discovered, &capabilities, &groups,
                                0) != 0 ||
            strcmp(discovered, format) != 0)
            continue;
        if (++matches != 1u) break;
        /* The trust/filename probe above already bounds names to 59
         * characters. Re-check here before copying because dirent names
         * are externally supplied and -Werror must not permit truncation. */
        const size_t name_length = strnlen(item->d_name, sizeof(selected));
        if (name_length == 0u || name_length >= sizeof(selected)) {
            matches = 2u;
            break;
        }
        memcpy(selected, item->d_name, name_length + 1u);
        selected_capabilities = capabilities;
    }
    closedir(entries);
    const size_t selected_length = strlen(selected);
    /* Count every trusted provider, including CORE/VOLUME-only modules.
     * A second owner of the same format is ambiguous even if it lacks
     * DEFRAG. Filtering it out first would silently choose a writer. */
    if(matches>1u)return -2; /* duplicate descriptor ownership */
    if (matches != 1u || (require_defrag && !selected_capabilities) ||
        selected_length == 0u || selected_length + 1u > capacity)
        return -1;
    memcpy(filename, selected, selected_length + 1u);
    if(actual_capabilities)*actual_capabilities=selected_capabilities;
    return 0;
}
/* Mutation operations demand a real DEFRAG group. Read-only volume queries
 * can use mounted CORE + VOLUME providers before DEFRAG is implemented. */
int ifs_filesystem_module_find_v2(const char *format, char *filename,
                                  size_t capacity)
{
    return ifs_filesystem_module_find_selected_v2(
        format, filename, capacity, 1,NULL);
}
int ifs_filesystem_module_find_readonly_v2(const char *format, char *filename,
                                           size_t capacity)
{
    return ifs_filesystem_module_find_selected_v2(
        format, filename, capacity, 0,NULL);
}

/* Capabilities are obtained from the same unique, authenticated provider
 * descriptor that the operation engine uses; registry hints are not proof. */
int ifs_filesystem_module_capabilities_v2(const char *format,uint64_t *capabilities)
{
    char filename[64];
    if(!capabilities)return -1;
    *capabilities=0u;
    return ifs_filesystem_module_find_selected_v2(
        format,filename,sizeof(filename),0,capabilities);
}

/* Strict Common V2 provider execution. No format-specific worker, private
 * filesystem ABI, or automatic compatibility fallback is permitted here.
 * This bridge reuses the already exclusive, identity-bound native Target. */
typedef struct { int argc; char **argv; const char *format; FILE **capture; } Command;
static void *v2_allocate(void *opaque,size_t bytes) {
    (void)opaque;
    return malloc(bytes);
}
static void v2_release(void *opaque,void *p) {
    (void)opaque;
    free(p);
}
static int v2_read_at(void *opaque,uint64_t at,void *out,uint32_t bytes) {
    IfsFilesystemTargetV2 *b=opaque;
    return b->services->read_at(b->services->context,&b->target,at,out,bytes);
}
static int v2_write_at(void *opaque,uint64_t at,const void *source,uint32_t bytes) {
    IfsFilesystemTargetV2 *b=opaque;
    return b->services->write_at(b->services->context,&b->target,at,source,bytes);
}
static int v2_sync(void *opaque) {
    IfsFilesystemTargetV2 *b=opaque;
    return b->services->sync_target(b->services->context,&b->target);
}
static int v2_cancelled(void *opaque) {
    IfsFilesystemTargetV2 *b=opaque;
    return b->services->stop_requested(b->services->context);
}
static uint64_t v2_clock_ns(clockid_t clock) {
    struct timespec t;
    if(clock_gettime(clock,&t)!=0 || t.tv_sec<0 || t.tv_nsec<0 ||
       t.tv_nsec>=1000000000 || (uint64_t)t.tv_sec>UINT64_MAX/UINT64_C(1000000000))
        return 0u;
    uint64_t seconds=(uint64_t)t.tv_sec*UINT64_C(1000000000);
    if((uint64_t)t.tv_nsec>UINT64_MAX-seconds)return 0u;
    return seconds+(uint64_t)t.tv_nsec;
}
static uint64_t v2_monotonic(void *opaque) {
    (void)opaque;
    return v2_clock_ns(CLOCK_MONOTONIC);
}
static uint64_t v2_realtime(void *opaque) {
    (void)opaque;
    return v2_clock_ns(CLOCK_REALTIME);
}
static int v2_random(void *opaque,void *destination,uint32_t bytes) {
    unsigned char *out=destination;
    (void)opaque;
    if(!out && bytes)return -1;
    while(bytes) {
        ssize_t count=getrandom(out,bytes,0);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return -1;
        out+=(size_t)count;
        bytes-=(uint32_t)count;
    }
    return 0;
}
int ifs_filesystem_target_runtime_v2(IfsFilesystemTargetV2 *binding,
                                     int writable, uint32_t requested_sector,
                                     IfsFsHostRuntimeV2 *runtime) {
    Target *t=binding->target.handle;
    if(!t || (writable && !t->device.writable))return -1;
    /* Do not advertise clock services when their initial samples fail. */
    if(!v2_clock_ns(CLOCK_MONOTONIC) || !v2_clock_ns(CLOCK_REALTIME))return -1;
    uint32_t logical=512u,physical=512u;
    if(t->device.is_block) {
        int logical_kernel=0,physical_kernel=0;
        if(ioctl(t->device.fd,BLKSSZGET,&logical_kernel)!=0 ||
           logical_kernel<512 || logical_kernel>65536 ||
           (logical_kernel & (logical_kernel-1)) ||
           (requested_sector && requested_sector!=(uint32_t)logical_kernel))
            return -1;
        logical=(uint32_t)logical_kernel;
        if(ioctl(t->device.fd,BLKPBSZGET,&physical_kernel)==0 &&
           physical_kernel>=logical_kernel && physical_kernel<=65536 &&
           !(physical_kernel & (physical_kernel-1)))
            physical=(uint32_t)physical_kernel;
        else physical=logical;
    } else if(requested_sector) logical=physical=requested_sector;
    *runtime=(IfsFsHostRuntimeV2){
        .abi_version=IFS_FS_HOST_ABI_V2,
        .structure_bytes=sizeof(*runtime),
        .capabilities=(writable ? IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE :
                                  IFS_FS_HOST_READ_ONLY)|IFS_FS_HOST_CANCEL|IFS_FS_HOST_CLOCK,
        .volume_bytes=binding->target.bytes,
        .logical_sector_bytes=logical,
        .physical_sector_bytes=physical,
        .maximum_transfer_bytes=65536u,
        .context=binding,
        .read_at=v2_read_at,
        .write_at=writable?v2_write_at:NULL,
        .flush=writable?v2_sync:NULL,
        .allocate=v2_allocate,
        .release=v2_release,
        .cancelled=v2_cancelled,
        .monotonic_ns=v2_monotonic,
        .realtime_ns=v2_realtime,
        .random_bytes=v2_random
    };
    return ifs_fs_host_v2_valid(runtime)?0:-1;
}
static int v2_path_valid(const char *path) {
    if (!path || path[0]!='/') return 0;
    const size_t n=strnlen(path,4096u);
    if(!n || n>=4096u)return 0;
    for(size_t i=0;i<n;++i) {
        if((unsigned char)path[i]<32u || path[i]=='\\')return 0;
        if(path[i]=='/' && path[i+1]=='/')return 0;
        if(path[i]=='.' && (i==1u || path[i-1]=='/') &&
           (path[i+1]=='/' || path[i+1]==0 ||
            (path[i+1]=='.' &&
             (path[i+2]=='/' || path[i+2]==0))))
            return 0;
    }
    return 1;
}
/* A FAT provider's root DEFRAG.MAP describes allocated data runs. Its
 * non-root maps describe file extents in logical order. The host transports
 * these canonical ranges, never parses disk metadata or follows FAT chains.
 * Keep all output private until the map and mounted lifecycle complete. */
typedef struct {
    const IfsFsCoreOpsV2 *core;
    const IfsFsDefragOpsV2 *defrag;
    void *volume;
    FILE *out;
    uint64_t unit, total, base, allocated, regular, fragmented, dirs, entries;
    uint64_t fragmented_dirs;
    int have_base, comma;
} V2FatMap;

static int v2_fat_range(V2FatMap *m,uint64_t start,uint64_t end) {
    if(start>=end || end>m->total)return -1;
    if(fprintf(m->out,"%s[%llu,%llu]",m->comma?",":"",
          (unsigned long long)start,(unsigned long long)end)<0)return -1;
    m->comma=1;
    return 0;
}
static int v2_fat_root(V2FatMap *m,uint64_t capacity) {
    uint64_t first=0u,last=0u;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsExtentV2 ext[128]={{0}};
        uint32_t n=0u;
        int rc=m->defrag->map(m->volume,"/",first,ext,128u,&n);
        if(rc==IFS_FS_END)return n==0u?0:-1;
        if(rc!=IFS_FS_OK || !n || n>128u || first>UINT64_MAX-n)return -1;
        for(uint32_t i=0u;i<n;++i) {
            const IfsFsExtentV2 *e=&ext[i];
            if(e->flags || e->reserved || !e->length || e->logical_offset%m->unit ||
               e->length%m->unit || e->physical_offset<e->logical_offset ||
               e->physical_offset>capacity ||
               e->length>capacity-e->physical_offset)return -1;
            uint64_t base=e->physical_offset-e->logical_offset;
            if(m->have_base && m->base!=base)return -1;
            m->base=base;m->have_base=1;
            uint64_t start=e->logical_offset/m->unit,count=e->length/m->unit;
            if(start<last || start>=m->total || count>m->total-start ||
               count>m->total-m->allocated)return -1;
            last=start+count;m->allocated+=count;
            if(v2_fat_range(m,start,last))return -1;
        }
        first+=n;
    }
    return -1;
}
static int v2_fat_file(V2FatMap *m,const char *path,uint64_t size) {
    uint64_t first=0u,logical=0u,previous_end=0u;
    int fragmented=0;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsExtentV2 ext[128]={{0}};
        uint32_t n=0u;
        int rc=m->defrag->map(m->volume,path,first,ext,128u,&n);
        if(rc==IFS_FS_END) {
            if(n || logical!=size)return -1;
            ++m->regular;
            if(fragmented)++m->fragmented;
            return 0;
        }
        if(rc!=IFS_FS_OK || !n || n>128u || first>UINT64_MAX-n)return -1;
        for(uint32_t i=0u;i<n;++i) {
            const IfsFsExtentV2 *e=&ext[i];
            if(e->flags || e->reserved || !m->have_base || e->physical_offset<m->base ||
               e->logical_offset!=logical || !e->length ||
               logical>size || e->length>size-logical ||
               (e->physical_offset-m->base)%m->unit ||
               (e->length%m->unit && e->length!=size-logical) ||
               e->physical_offset>UINT64_MAX-e->length)return -1;
            uint64_t start=(e->physical_offset-m->base)/m->unit;
            uint64_t count=e->length/m->unit+(e->length%m->unit!=0u);
            if(start>=m->total || count>m->total-start)return -1;
            /* Red cells represent only displaced extents, not the first run. */
            if(logical && e->physical_offset!=previous_end) {
                fragmented=1;
                if(v2_fat_range(m,start,start+count))return -1;
            }
            previous_end=e->physical_offset+e->length;
            logical+=e->length;
        }
        first+=n;
    }
    return -1;
}
/* Directory allocation has no portable size field in FAT STAT.
 * Only the provider's complete non-root MAP supplies authoritative
 * bounds. Count real discontinuities; never substitute a zero count
 * when a directory cannot be mapped. */
static int v2_fat_directory_extents(V2FatMap *m,const char *path) {
    uint64_t first=0u,logical=0u,previous_end=0u;
    int fragmented=0;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsExtentV2 ext[128]={{0}};
        uint32_t n=0u;
        int rc=m->defrag->map(m->volume,path,first,ext,128u,&n);
        if(rc==IFS_FS_END) {
            if(n || !logical)return -1;
            if(fragmented)++m->fragmented_dirs;
            return 0;
        }
        if(rc!=IFS_FS_OK || !n || n>128u || first>UINT64_MAX-n)return -1;
        for(uint32_t i=0u;i<n;++i) {
            const IfsFsExtentV2 *e=&ext[i];
            if(e->flags || e->reserved || !m->have_base ||
               e->physical_offset<m->base || e->logical_offset!=logical ||
               !e->length || e->length%m->unit ||
               (e->physical_offset-m->base)%m->unit ||
               e->physical_offset>UINT64_MAX-e->length ||
               logical>UINT64_MAX-e->length)return -1;
            const uint64_t start=(e->physical_offset-m->base)/m->unit;
            const uint64_t count=e->length/m->unit;
            if(start>=m->total || count>m->total-start ||
               logical/m->unit>m->total-count)return -1;
            if(logical && e->physical_offset!=previous_end) {
                fragmented=1;
                if(v2_fat_range(m,start,start+count))return -1;
            }
            previous_end=e->physical_offset+e->length;
            logical+=e->length;
        }
        first+=n;
    }
    return -1;
}
static int v2_fat_directory(V2FatMap *m,const char *path,unsigned depth) {
    if(depth>64u)return -1;
    for(uint32_t index=0u;index<4194304u;++index) {
        char name[4096]={0},child[4096];
        IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=name,
                       .name_capacity=sizeof(name)};
        int rc=m->core->directory_entry(m->volume,path,index,&e);
        if(rc==IFS_FS_END)return 0;
        if(rc!=IFS_FS_OK || ++m->entries>4194304u ||
           !e.name_bytes || e.name_bytes>=sizeof(name) ||
           name[e.name_bytes] || strlen(name)!=e.name_bytes ||
           strchr(name,'/') || strchr(name,'\\'))return -1;
        if(!strcmp(name,".") || !strcmp(name,".."))continue;
        int len=snprintf(child,sizeof(child),"%s%s%s",path,
                         !strcmp(path,"/")?"":"/",name);
        if(len<=0 || (size_t)len>=sizeof(child))return -1;
        if(e.type==IFS_FS_ENTRY_DIRECTORY) {
            ++m->dirs;
            if(v2_fat_directory_extents(m,child) ||
               v2_fat_directory(m,child,depth+1u))return -1;
        } else if(e.type==IFS_FS_ENTRY_FILE) {
            if(v2_fat_file(m,child,e.size_bytes))return -1;
        } else return -1;
    }
    return -1;
}
static int v2_readonly_plan(const IfsFsDefragOpsV2 *,void *,uint32_t);
static int v2_fat_extent_json(FILE *out,const IfsFsModuleApiV2 *api,
          const IfsFsCoreOpsV2 *core,const IfsFsDefragOpsV2 *defrag,
          void *volume,const IfsFsVolumeInfoV2 *info,uint64_t capacity) {
    if(!(core->header.capabilities&IFS_FS_CORE_LIST) || !core->directory_entry ||
       !info->block_bytes || info->total_bytes>capacity ||
       info->free_bytes>info->total_bytes ||
       info->total_bytes%info->block_bytes ||
       info->free_bytes%info->block_bytes)return -1;
    V2FatMap m={.core=core,.defrag=defrag,.volume=volume,.out=out,
       .unit=info->block_bytes,.total=info->total_bytes/info->block_bytes};
    if(!m.total || fprintf(out,"{\"filesystem\":\"%s\",\"abi\":2,"
       "\"unit_size\":%llu,\"total_units\":%llu,\"free_units\":%llu,"
       "\"allocated_ranges\":[",api->format_id,(unsigned long long)m.unit,
       (unsigned long long)m.total,
       (unsigned long long)(info->free_bytes/m.unit))<0)return -1;
    if(v2_fat_root(&m,capacity) ||
       m.allocated!=m.total-info->free_bytes/m.unit ||
       (m.have_base && (m.base>capacity || info->total_bytes>capacity-m.base)))
        return -1;
    if(fputs("],\"fragmented_ranges\":[",out)==EOF)return -1;
    m.comma=0;
    if(v2_fat_directory(&m,"/",0u))return -1;
    if(fprintf(out,"],\"regular_files\":%llu,\"fragmented_files\":%llu,"
       "\"directories\":%llu,\"directory_fragmentation_available\":true,"
       "\"physical_geometry_available\":%s,\"data_start_bytes\":%llu,"
       "\"device_bytes\":%llu,\"defrag_qualified\":%s,\"growth_qualified\":%s",
       (unsigned long long)m.regular,(unsigned long long)m.fragmented,
       (unsigned long long)m.dirs,
       m.have_base?"true":"false",(unsigned long long)m.base,
       (unsigned long long)capacity,
       v2_readonly_plan(defrag,volume,0u)?"true":"false",
       v2_readonly_plan(defrag,volume,10u)?"true":"false")<0)return -1;
    if(fprintf(out,",\"recovery_qualified\":%s",
        (defrag->header.capabilities&IFS_FS_DEFRAG_RECOVER)?"true":"false")<0)return -1;
    if(fprintf(out,",\"fragmented_directories\":%llu",
        (unsigned long long)m.fragmented_dirs)<0)return -1;
    return fputs("}\n",out)==EOF || fflush(out)!=0?-1:0;
}


/* PLAN is an independent provider-owned, read-only qualification. Never
 * infer a writable layout from an advertised capability or from an incomplete
 * allocation map. A fresh plan is still required under the exclusive write
 * lease before executing any mutation. */
static int v2_readonly_plan(const IfsFsDefragOpsV2 *defrag,void *volume,
                            uint32_t policy)
{
    const uint64_t required=IFS_FS_DEFRAG_PLAN|IFS_FS_DEFRAG_EXECUTE|
                            IFS_FS_DEFRAG_VERIFY;
    if(!defrag || (defrag->header.capabilities&required)!=required ||
       !defrag->plan || !defrag->execute || !defrag->verify ||
       !defrag->release_plan)return 0;
    void *plan=NULL;
    const int rc=defrag->plan(volume,"/",policy,&plan);
    if(rc!=IFS_FS_OK || !plan)return 0;
    defrag->release_plan(volume,plan);
    return 1;
}

/* Common V2 guarantees file extent maps, not a whole-volume free-space or
 * metadata map. Preserve physical byte ranges exactly and mark everything
 * outside those ranges unknown in the consumer. Never infer free space
 * from an absent file extent or use a format-specific disk parser. */
typedef struct {
    const IfsFsCoreOpsV2 *core;
    const IfsFsDefragOpsV2 *defrag;
    const IfsFsHostRuntimeV2 *runtime;
    void *volume;
    FILE *out;
    uint32_t unit;
    uint64_t files, fragmented, dirs, entries, extents;
    int comma;
} V2Catalogue;
static int v2_catalogue_file(V2Catalogue *m,const char *path,uint64_t bytes)
{
    uint64_t first=0u,logical=0u,previous_end=0u;
    int split=0;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsExtentV2 ext[32]={{0}};
        uint32_t n=0u;
        if(m->runtime->cancelled && m->runtime->cancelled(m->runtime->context))
            return -1;
        const int rc=m->defrag->map(m->volume,path,first,ext,32u,&n);
        if(rc==IFS_FS_END) {
            if(n || logical!=bytes)return -1;
            ++m->files;
            if(split)++m->fragmented;
            return 0;
        }
        if(rc!=IFS_FS_OK || !n || n>32u || first>UINT64_MAX-n)return -1;
        for(uint32_t i=0u;i<n;++i) {
            const IfsFsExtentV2 *e=&ext[i];
            if(e->flags || e->reserved || !e->length ||
               e->logical_offset!=logical || logical>bytes ||
               e->length>bytes-logical ||
               e->physical_offset>m->runtime->volume_bytes ||
               e->length>m->runtime->volume_bytes-e->physical_offset ||
               ++m->extents>1048576u)return -1;
            const int displaced=logical &&
                (e->physical_offset<previous_end ||
                 e->physical_offset-previous_end>=m->unit);
            const uint64_t end=e->physical_offset+e->length;
            if(fprintf(m->out,"%s[%llu,%llu,%u]",m->comma?",":"",
                 (unsigned long long)e->physical_offset,
                 (unsigned long long)end,(unsigned)displaced)<0)return -1;
            m->comma=1;
            if(displaced)split=1;
            previous_end=end;
            logical+=e->length;
        }
        first+=n;
    }
    return -1;
}
static int v2_catalogue_directory(V2Catalogue *m,const char *path,unsigned depth)
{
    if(depth>64u)return -1;
    for(uint32_t index=0u;index<1048576u;++index) {
        char name[4096]={0},child[4096];
        IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=name,
                       .name_capacity=sizeof(name)};
        if(m->runtime->cancelled && m->runtime->cancelled(m->runtime->context))
            return -1;
        const int rc=m->core->directory_entry(m->volume,path,index,&e);
        if(rc==IFS_FS_END)return 0;
        if(rc!=IFS_FS_OK || ++m->entries>1048576u ||
           !e.name_bytes || e.name_bytes>=sizeof(name) ||
           name[e.name_bytes] || strlen(name)!=e.name_bytes ||
           strchr(name,'/') || strchr(name,'\\'))return -1;
        if(!strcmp(name,".") || !strcmp(name,".."))continue;
        const int len=snprintf(child,sizeof(child),"%s%s%s",path,
                               !strcmp(path,"/")?"":"/",name);
        if(len<=0 || (size_t)len>=sizeof(child))return -1;
        if(e.type==IFS_FS_ENTRY_DIRECTORY) {
            ++m->dirs;
            if(v2_catalogue_directory(m,child,depth+1u))return -1;
        } else if(e.type==IFS_FS_ENTRY_FILE) {
            if(v2_catalogue_file(m,child,e.size_bytes))return -1;
        } else if(e.type!=IFS_FS_ENTRY_LINK)return -1;
    }
    return -1;
}
/* Transport only the canonical provider-owned physical allocation group.
 * This is independent of object extents. Missing groups leave the old
 * file catalogue incomplete; a malformed advertised group fails closed.
 */
static int v2_allocation_json(FILE *out,const IfsFsAllocationOpsV2 *ops,
    void *volume,const IfsFsVolumeInfoV2 *info,const IfsFsHostRuntimeV2 *runtime)
{
    uint64_t cursor=0u,unknown=0u,free_bytes=0u,entries=0u;
    if(!ops || !ops->map ||
       fputs(",\"allocation_ranges\":[",out)==EOF)return -1;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsAllocationRangeV2 ranges[128]={{0}};
        uint32_t count=0u;
        uint64_t next=cursor;
        if(runtime->cancelled && runtime->cancelled(runtime->context))return -1;
        const int rc=ops->map(volume,cursor,ranges,128u,&count);
        if(!ifs_fs_allocation_page_v2_valid(runtime->volume_bytes,cursor,rc,
                ranges,128u,count,&next,&unknown))return -1;
        if(rc==IFS_FS_END) {
            /* The volume's accounting must agree with a complete map.
             * For partial maps, known free cannot exceed measured free. */
            if(free_bytes>info->free_bytes ||
               (!unknown && free_bytes!=info->free_bytes))return -1;
            return fprintf(out,"],\"allocation_unknown_bytes\":%llu,"
                "\"allocation_free_bytes\":%llu",
                (unsigned long long)unknown,(unsigned long long)free_bytes)<0?-1:0;
        }
        for(uint32_t i=0u;i<count;++i) {
            const IfsFsAllocationRangeV2 *r=&ranges[i];
            if(r->state==IFS_FS_SPACE_FREE)free_bytes+=r->length;
            if(entries>=1048576u || fprintf(out,"%s[%llu,%llu,%u]",
               entries?",":"",(unsigned long long)r->physical_offset,
               (unsigned long long)(r->physical_offset+r->length),r->state)<0)return -1;
            ++entries;
        }
        cursor=next;
    }
    return -1;
}
static int v2_catalogue_json(FILE *out,const IfsFsModuleApiV2 *api,
    const IfsFsCoreOpsV2 *core,const IfsFsDefragOpsV2 *defrag,void *volume,
    const IfsFsVolumeInfoV2 *info,const IfsFsHostRuntimeV2 *runtime)
{
    if(!out || !(core->header.capabilities&IFS_FS_CORE_LIST) ||
       !core->directory_entry || !info->block_bytes ||
       !info->total_bytes || info->total_bytes>runtime->volume_bytes ||
       info->free_bytes>info->total_bytes)return -1;
    V2Catalogue m={.core=core,.defrag=defrag,.runtime=runtime,
        .volume=volume,.out=out,.unit=info->block_bytes};
    if(fprintf(out,"{\"filesystem\":\"%s\",\"abi\":2,"
       "\"device_bytes\":%llu,\"file_extents\":[",api->format_id,
       (unsigned long long)runtime->volume_bytes)<0 ||
       v2_catalogue_directory(&m,"/",0u))return -1;
    /* No directory allocation contract is assumed. Zero directories is
     * the only case where directory fragmentation is exactly zero. */
    if(fprintf(out,"],\"regular_files\":%llu,\"fragmented_files\":%llu,"
       "\"directories\":%llu,\"directory_fragmentation_available\":%s,"
       "\"provider_defrag_capabilities\":%llu",
       (unsigned long long)m.files,(unsigned long long)m.fragmented,
       (unsigned long long)m.dirs,m.dirs?"false":"true",
       (unsigned long long)defrag->header.capabilities)<0)return -1;
    const IfsFsAllocationOpsV2 *allocation=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    if(allocation && v2_allocation_json(out,allocation,volume,info,runtime))
        return -1;
    /* A complete file catalogue need not imply complete volume allocation
     * accounting. Ask the authoritative provider to qualify both policies
     * on this read-only session; do not execute, verify a mutation, or write.
     * Refusal remains a refusal, not an invitation to use a legacy worker. */
    const int packed=v2_readonly_plan(defrag,volume,0u);
    const int growth=v2_readonly_plan(defrag,volume,10u);
    if(fprintf(out,",\"provider_plan_preflight\":true,\"defrag_qualified\":%s,"
        "\"growth_qualified\":%s",packed?"true":"false",
        growth?"true":"false")<0)return -1;
    if(fprintf(out,",\"recovery_qualified\":%s",
        (defrag->header.capabilities&IFS_FS_DEFRAG_RECOVER)?"true":"false")<0)return -1;
    if(!m.dirs && fputs(",\"fragmented_directories\":0",out)==EOF)return -1;
    return fputs("}\n",out)==EOF || fflush(out)!=0?-1:0;
}

/* Transport the actual provider-owned extent pages. This is not a complete
 * allocation map: root semantics are provider-defined, and the host never
 * infers free space from unreported ranges. Output is published only after
 * the terminal END page and a successful read-only unmount. */
static int v2_path_extent_json(FILE *out,const IfsFsModuleApiV2 *api,
    const IfsFsDefragOpsV2 *defrag,void *volume,const IfsFsHostRuntimeV2 *runtime,
    const char *path)
{
    uint64_t first=0u;
    if(!out || fprintf(out,"{\"filesystem\":\"%s\",\"abi\":2,\"extents\":[",
        api->format_id)<0)return -1;
    for(unsigned page=0u;page<131072u;++page) {
        IfsFsExtentV2 extents[32]={{0}};
        uint32_t count=0u;
        if(runtime->cancelled && runtime->cancelled(runtime->context))return -1;
        const int rc=defrag->map(volume,path,first,extents,32u,&count);
        if(rc==IFS_FS_END) {
            if(count)return -1;
            return fprintf(out,"],\"extent_count\":%llu}\n",
                (unsigned long long)first)<0 || fflush(out)!=0?-1:0;
        }
        if(rc!=IFS_FS_OK || !count || count>32u || first>UINT64_MAX-count)
            return -1;
        for(uint32_t i=0u;i<count;++i) {
            const IfsFsExtentV2 *e=&extents[i];
            if(e->reserved || !e->length ||
               e->logical_offset>UINT64_MAX-e->length ||
               e->physical_offset>runtime->volume_bytes ||
               e->length>runtime->volume_bytes-e->physical_offset)
                return -1;
            if(fprintf(out,"%s[%llu,%llu,%llu,%u]",(first || i)?",":"",
                (unsigned long long)e->logical_offset,
                (unsigned long long)e->physical_offset,
                (unsigned long long)e->length,(unsigned)e->flags)<0)return -1;
        }
        first+=count;
    }
    return -1;
}

static int run_common_v2(const IfsDefragHostV2 *services,void *symbol,void *user) {
    const Command *command=user;
    IfsFsModuleApiEntryV2 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"module entry width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry?entry():NULL;
    if(!api || !ifs_fs_api_v2_valid(api,command->format) ||
       !(api->runtime_profiles & IFS_FS_PROFILE_HOSTED)) {
        fputs("Provider does not implement the exact strict Common V2 ABI\n",stderr);
        return 1;
    }
    const IfsFsCoreOpsV2 *core=
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsDefragOpsV2 *defrag=
        (const IfsFsDefragOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    if(!core || !(core->header.capabilities & IFS_FS_CORE_PROBE) ||
       !core->probe) {
        fputs("V2 provider lacks a native CORE probe\n",stderr);
        return 1;
    }
    if(command->argc<3 || !command->argv[1] || !command->argv[2])return 2;
    const char *operation=command->argv[1],*path="/",*confirmed_target=NULL;
    int explicit_path=0;
    uint32_t requested_sector=0u;
    int explicit_write=0;
    /* Internal, silent read-only identification. The universal dispatcher
     * uses provider-owned CORE.PROBE to select UFS1 or UFS2; never parse
     * filesystem magic or call retired V1 entry points in Defragmenter. */
    const int probing=!strcmp(operation,"probe-only");
    if(probing && command->argc!=3)return 2;
    const int reporting=!strcmp(operation,"volume-info");
    const int recovering=!strcmp(operation,"recover");
    const int fat=!strcmp(api->format_id,"fat12") ||
        !strcmp(api->format_id,"fat16") || !strcmp(api->format_id,"fat32");
    const int analysis_request=!strcmp(operation,"analyse") || !strcmp(operation,"analyse-json");
    const int extent_json=!strcmp(operation,"map-extents") || (analysis_request && fat);
    const int catalogue=!strcmp(operation,"map-catalogue") || (analysis_request && !fat);
    const int path_map=!strcmp(operation,"map");
    if(catalogue && command->argc!=3)return 2;
    if(extent_json && (command->argc!=3 || !fat))return 2;
    const int analysing=probing || reporting || extent_json || catalogue || path_map;
    const int defragging=!strcmp(operation,"defrag");
    const int growing=!strcmp(operation,"growth-defrag");
    if(!recovering && !analysing && !defragging && !growing)return 2;
    const IfsFsVolumeOpsV2 *v2_volume =
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(
            api, IFS_FS_GROUP_VOLUME);
    if(reporting) {
        if(!v2_volume ||
           !(v2_volume->header.capabilities & IFS_FS_VOLUME_QUERY) ||
           !v2_volume->query) {
            fputs("V2 provider does not support a real volume query\n",stderr);
            return 2;
        }
    } else if(!probing && !defrag) {
        fputs("V2 provider lacks the required DEFRAG group\n",stderr);
        return 2;
    }
    for(int i=3;i<command->argc;++i) {
        const char *arg=command->argv[i];
        if(!strcmp(arg,"--write")) {
            if(explicit_write)return 2;
            explicit_write=1;
        } else if(!strcmp(arg,"--confirm")) {
            if(confirmed_target || ++i==command->argc)return 2;
            confirmed_target=command->argv[i];
        } else if(!strcmp(arg,"--path")) {
            if(explicit_path || ++i==command->argc)return 2;
            path=command->argv[i];
            explicit_path=1;
        } else if(!strncmp(arg,"--path=",7u)) {
            if(explicit_path)return 2;
            path=arg+7;
            explicit_path=1;
        } else if(!strncmp(arg,"--sector-bytes=",15u)) {
            if(requested_sector)return 2;
            char *end=NULL;
            errno=0;
            unsigned long v=strtoul(arg+15,&end,10);
            if(errno || !end || *end || v<512u || v>65536u ||
               (v & (v-1u)))return 2;
            requested_sector=(uint32_t)v;
        } else {
            fprintf(stderr,"Unsupported Common V2 argument: %s\n",arg);
            return 2;
        }
    }
    /* A trusted V2 module may mutate real offline media. The host, not the
     * provider, must require explicit, target-bound destructive consent. */
    if((!analysing && (!explicit_write || !confirmed_target ||
         strcmp(confirmed_target,command->argv[2]))) ||
       (analysing && (explicit_write || confirmed_target))) {
        fputs("Common V2 mutation requires --write --confirm DEVICE\n",stderr);
        return 2;
    }
    if((reporting && explicit_path) ||
       (!recovering && !v2_path_valid(path))) {
        fputs("Common V2 mapping/planning requires a valid absolute filesystem path\n",stderr);
        return 2;
    }
    /* Recovery is independent. Do not refuse a valid PLAN/EXECUTE/VERIFY
     * provider just because it does not implement journal recovery. */
    const uint64_t required=recovering?IFS_FS_DEFRAG_RECOVER:
        analysing?IFS_FS_DEFRAG_MAP:
        IFS_FS_DEFRAG_PLAN|IFS_FS_DEFRAG_EXECUTE|IFS_FS_DEFRAG_VERIFY;
    if(!probing && !reporting &&
       (defrag->header.capabilities & required)!=required) {
        fputs("Provider does not qualify the requested strict V2 operation\n",stderr);
        return 2;
    }
    const int writable=!analysing;
    IfsFilesystemTargetV2 b={.services=services};
    if(services->open_target(services->context,command->argv[2],writable,
                             NULL,0u,&b.target)) {
        fputs("Cannot obtain trusted exclusive V2 storage lease\n",stderr);
        return 1;
    }
    int result=1;
    IfsFsHostRuntimeV2 h;
    if(ifs_filesystem_target_runtime_v2(&b,writable,requested_sector,&h))
        goto cleanup_target;
    uint32_t variant=0u;
    if(core->probe(&h,&variant)!=IFS_FS_OK) {
        if(!probing)
            fputs("V2 provider rejected the target's filesystem geometry\n",stderr);
        goto cleanup_target;
    }
    if(probing) {
        result=0;
        goto cleanup_target;
    }
    if(!(core->header.capabilities & IFS_FS_CORE_MOUNT) ||
       !core->mount || !core->unmount) {
        fputs("V2 provider lacks a mounted CORE lifecycle\n",stderr);
        goto cleanup_target;
    }
    IfsFsVolumeInfoV2 info={0};
    info.structure_bytes=sizeof(info);
    void *volume=NULL;
    if(core->mount(&h,&volume,&info)!=IFS_FS_OK || !volume) {
        fputs("Native V2 filesystem mount failed\n",stderr);
        goto cleanup_target;
    }
    int operation_ok=0;
    FILE *extent_output=NULL;
    IfsFsVolumeInfoV2 reported={0};
    if(reporting) {
        reported.structure_bytes=sizeof(reported);
        operation_ok=v2_volume->query(volume,&reported)==IFS_FS_OK &&
            reported.structure_bytes>=sizeof(reported) &&
            reported.total_bytes<=h.volume_bytes &&
            reported.free_bytes<=reported.total_bytes &&
            reported.block_bytes>=512u;
        if(!operation_ok)
            fputs("Common V2 volume query returned invalid geometry\n",stderr);
    } else if(recovering) {
        uint64_t repaired=0u;
        operation_ok=defrag->recover(volume,0u,&repaired)==IFS_FS_OK;

    } else if(catalogue) {
        extent_output=tmpfile();
        operation_ok=extent_output && v2_catalogue_json(extent_output,api,core,defrag,volume,&info,&h)==0;
        if(!operation_ok)fputs("Common V2 file extent catalogue failed\n",stderr);
    } else if(extent_json) {
        extent_output=tmpfile();
        operation_ok=extent_output &&
            v2_fat_extent_json(extent_output,api,core,defrag,volume,&info,h.volume_bytes)==0;
        if(!operation_ok)fputs("Common V2 FAT extent catalogue failed\n",stderr);
    } else if(path_map) {
        extent_output=tmpfile();
        operation_ok=extent_output &&
            v2_path_extent_json(extent_output,api,defrag,volume,&h,path)==0;
        if(!operation_ok)fputs("Common V2 path extent map failed\n",stderr);
    } else {
        void *plan=NULL;
        const uint32_t policy=growing?10u:0u;
        int rc=defrag->plan(volume,path,policy,&plan);
        if(rc==IFS_FS_OK && plan) {
            uint64_t moved=0u;
            rc=defrag->execute(volume,plan,&moved);
            if(rc==IFS_FS_OK)rc=defrag->verify(volume,plan);
            operation_ok=rc==IFS_FS_OK;
            defrag->release_plan(volume,plan);

        }
    }
    /* Always unmount; never report successful mutation until both provider
     * volume sync and exclusive descriptor durability have succeeded. */
    const IfsFsVolumeOpsV2 *ops=
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(writable && operation_ok && ops && ops->sync)
        operation_ok=ops->sync(volume)==IFS_FS_OK;
    if(core->unmount(volume)!=IFS_FS_OK)operation_ok=0;
    if(writable && v2_sync(&b))operation_ok=0;
    result=operation_ok?0:1;
    /* Publish geometry only after the provider has successfully released its
     * session; callers must never parse a successful JSON record from a
     * failed or incompletely unmounted operation. */
    if(!result && reporting) {
        printf("{\"filesystem\":\"%s\",\"abi\":2,\"variant\":%u,"
               "\"block_bytes\":%u,\"total_bytes\":%llu,"
               "\"free_bytes\":%llu,\"source\":\"Common V2 VOLUME_QUERY\"}\n",
               api->format_id,reported.variant,reported.block_bytes,
               (unsigned long long)reported.total_bytes,
               (unsigned long long)reported.free_bytes);
    } else if(!result && (extent_json || catalogue || path_map)) {
        if(command->capture) {
            *command->capture=extent_output;
            extent_output=NULL;
        } else {
            if(fseek(extent_output,0,SEEK_SET)!=0)result=1;
            char buffer[16384];
            size_t count;
            while(!result && (count=fread(buffer,1,sizeof(buffer),extent_output))!=0u)
                if(fwrite(buffer,1,count,stdout)!=count)result=1;
            if(ferror(extent_output) || fflush(stdout)!=0)result=1;
        }
    } else if(!result) {
        fputs("Common V2 provider operation verified and durably synced\n",stdout);
    }
    if(extent_output)fclose(extent_output);
cleanup_target:
    services->close_target(services->context,&b.target);
    return result;
}
int ifs_filesystem_module_run(const char *format, const char *filename,
                              int argc, char **argv) {
    if(argc>=2 && (!strcmp(argv[1],"defrag") ||
       !strcmp(argv[1],"growth-defrag") || !strcmp(argv[1],"recover"))) {
        ld_stop_clear(); ld_stop_install_handlers(); ld_stop_report_ready();
    }
    Command command={argc,argv,format,NULL};
    return ifs_filesystem_module_call(format,filename,
        IFS_FS_COMMON_EXPORT_V2,run_common_v2,&command);
}


int ifs_filesystem_module_capture_map_v2(const char *format,const char *target,
                                         char **json,size_t maximum_bytes)
{
    if(!json)return -1;
    *json=NULL;
    if(!format || !target || !maximum_bytes)return -1;
    char filename[64]={0};
    if(ifs_filesystem_module_find_v2(format,filename,sizeof(filename))!=0)
        return -1;
    const int fat=!strcmp(format,"fat12") || !strcmp(format,"fat16") ||
                  !strcmp(format,"fat32");
    char *argv[]={(char *)"defragmenter-mapper",
                  (char *)(fat?"map-extents":"map-catalogue"),
                  (char *)target,NULL};
    FILE *capture=NULL;
    Command command={3,argv,format,&capture};
    const int rc=ifs_filesystem_module_call(format,filename,
        IFS_FS_COMMON_EXPORT_V2,run_common_v2,&command);
    int result=-1;
    char *buffer=NULL;
    if(rc || !capture)goto done;
    if(fseek(capture,0,SEEK_END)!=0)goto done;
    const long end=ftell(capture);
    if(end<=0 || (uint64_t)end>=maximum_bytes ||
       (uint64_t)end>SIZE_MAX-1u || fseek(capture,0,SEEK_SET)!=0)goto done;
    const size_t bytes=(size_t)end;
    buffer=malloc(bytes+1u);
    if(!buffer || fread(buffer,1,bytes,capture)!=bytes ||
       ferror(capture) || memchr(buffer,'\0',bytes))goto done;
    buffer[bytes]='\0';
    *json=buffer;
    buffer=NULL;
    result=0;
done:
    free(buffer);
    if(capture)fclose(capture);
    return result;
}

/* Resolve aliases exclusively through trusted Common V2 CORE.PROBE.
 * Refuse duplicate module ownership or multiple matching formats. */
int ifs_filesystem_module_detect_v2(
    const char *target,const char *const *candidates,size_t count,
    char *format,size_t format_capacity,char *filename,size_t filename_capacity)
{
    char match_format[IFS_FS_MAX_FORMAT_ID+1u]={0},match_file[64]={0};
    unsigned matches=0u;
    if(!target || !*target || !candidates || !count || !format ||
       !format_capacity || !filename || !filename_capacity)return -1;
    format[0]=filename[0]='\0';
    for(size_t i=0;i<count;++i) {
        char found[64]={0};
        if(!ifs_fs_format_id_valid(candidates[i]))return -1;
        const int lookup=ifs_filesystem_module_find_readonly_v2(candidates[i],found,sizeof(found));
        if(lookup==-2)return -2;
        if(lookup!=0)continue;
        char *argv[]={(char *)"defragmenter-v2-probe",(char *)"probe-only",(char *)target,NULL};
        if(ifs_filesystem_module_run(candidates[i],found,3,argv)!=0)continue;
        if(++matches!=1u)return -2;
        memcpy(match_format,candidates[i],strlen(candidates[i])+1u);
        memcpy(match_file,found,strlen(found)+1u);
    }
    if(matches!=1u || strlen(match_file)+1u>filename_capacity ||
       strlen(match_format)+1u>format_capacity)return -1;
    memcpy(format,match_format,strlen(match_format)+1u);
    memcpy(filename,match_file,strlen(match_file)+1u);
    return 0;
}

int ifs_filesystem_module_detect_ufs_v2(
    const char *target,char *format,size_t format_capacity,
    char *filename,size_t filename_capacity)
{
    static const char *const candidates[]={"ufs1","ufs2"};
    return ifs_filesystem_module_detect_v2(target,candidates,
        sizeof(candidates)/sizeof(candidates[0]),format,format_capacity,
        filename,filename_capacity);
}
