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
#include <linux/fs.h>
#include <sys/stat.h>
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
                               char format[16], uint64_t *capabilities) {
    if (!ifs_safe_module_filename(name) || !format || !capabilities)
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
        if (defrag && defrag->header.capabilities != 0u) {
            const size_t length = strnlen(api->format_id, 16u);
            if (length > 0u && length <= 15u) {
                memcpy(format, api->format_id, length + 1u);
                *capabilities = defrag->header.capabilities;
                result = 0;
            }
        }
    }
    dlclose(image);
    close(fd);
    return result;
}

/* Generic discovery is a read-only operation; a new trusted SO is visible
 * without adding any filesystem code or registry entry to Defragmenter. */
int ifs_filesystem_module_list_v2(void) {
    int directory = ifs_open_module_directory();
    if (directory < 0) {
        fputs("{\"schema\":1,\"modules\":[]}\n", stdout);
        return 0;
    }
    DIR *entries = fdopendir(directory);
    if (!entries) { close(directory); return 1; }
    int first = 1;
    fputs("{\"schema\":1,\"modules\":[", stdout);
    struct dirent *item;
    while ((item = readdir(entries)) != NULL) {
        char format[16];
        uint64_t capabilities = 0u;
        if (ifs_probe_module_v2(directory, item->d_name,
                                format, &capabilities) != 0)
            continue;
        if (!first) fputc(',', stdout);
        first = 0;
        printf("{\"format_id\":\"%s\",\"filename\":\"%s\","
               "\"capabilities\":%llu}",
               format, item->d_name, (unsigned long long)capabilities);
    }
    fputs("]}\n", stdout);
    closedir(entries);
    return ferror(stdout) ? 1 : 0;
}

/* Resolve by the module's exported format_id, not a guessed name or a host
 * filesystem registry. Duplicate providers fail closed. The dispatcher then
 * loads the returned trusted SO again and revalidates its ABI and identity. */
int ifs_filesystem_module_find_v2(const char *format, char *filename,
                                  size_t capacity) {
    if (!format || !filename || capacity == 0u) return -1;
    const size_t length = strnlen(format, 16u);
    if (length == 0u || length > 15u) return -1;
    for (size_t n = 0u; n < length; ++n) {
        const unsigned char ch = (unsigned char)format[n];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_'))
            return -1;
    }
    int directory = ifs_open_module_directory();
    if (directory < 0) return -1;
    DIR *entries = fdopendir(directory);
    if (!entries) { close(directory); return -1; }
    char selected[64] = "";
    unsigned int matches = 0u;
    struct dirent *item;
    while ((item = readdir(entries)) != NULL) {
        char discovered[16];
        uint64_t capabilities = 0u;
        if (ifs_probe_module_v2(directory, item->d_name,
                                discovered, &capabilities) != 0 ||
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
    }
    closedir(entries);
    const size_t selected_length = strlen(selected);
    if (matches != 1u || selected_length == 0u ||
        selected_length + 1u > capacity)
        return -1;
    memcpy(filename, selected, selected_length + 1u);
    return 0;
}

/* Strict Common V2 provider execution. No format-specific worker, private
 * filesystem ABI, or automatic compatibility fallback is permitted here.
 * This bridge reuses the already exclusive, identity-bound native Target. */
typedef struct {
    const IfsDefragHostV2 *services;
    IfsDefragTarget target;
} CommonV2Host;
typedef struct { int argc; char **argv; const char *format; } Command;
static void *v2_allocate(void *opaque,size_t bytes) {
    (void)opaque;
    return malloc(bytes);
}
static void v2_release(void *opaque,void *p) {
    (void)opaque;
    free(p);
}
static int v2_read_at(void *opaque,uint64_t at,void *out,uint32_t bytes) {
    CommonV2Host *b=opaque;
    return b->services->read_at(b->services->context,&b->target,at,out,bytes);
}
static int v2_write_at(void *opaque,uint64_t at,const void *source,uint32_t bytes) {
    CommonV2Host *b=opaque;
    return b->services->write_at(b->services->context,&b->target,at,source,bytes);
}
static int v2_sync(void *opaque) {
    CommonV2Host *b=opaque;
    return b->services->sync_target(b->services->context,&b->target);
}
static int v2_cancelled(void *opaque) {
    CommonV2Host *b=opaque;
    return b->services->stop_requested(b->services->context);
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
    if(!core || !defrag || !(core->header.capabilities & IFS_FS_CORE_MOUNT)) {
        fputs("V2 provider lacks mounted CORE and DEFRAG groups\n",stderr);
        return 1;
    }
    if(command->argc<3 || !command->argv[1] || !command->argv[2])return 2;
    const char *operation=command->argv[1],*path=NULL;
    uint32_t requested_sector=0u;
    const int recovering=!strcmp(operation,"recover");
    const int analysing=!strcmp(operation,"analyse") ||
        !strcmp(operation,"analyse-json") || !strcmp(operation,"map");
    const int defragging=!strcmp(operation,"defrag");
    const int growing=!strcmp(operation,"growth-defrag");
    if(!recovering && !analysing && !defragging && !growing)return 2;
    for(int i=3;i<command->argc;++i) {
        const char *arg=command->argv[i];
        if(!strcmp(arg,"--path")) {
            if(path || ++i==command->argc)return 2;
            path=command->argv[i];
        } else if(!strncmp(arg,"--path=",7u)) {
            if(path)return 2;
            path=arg+7;
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
    if(!recovering && !v2_path_valid(path)) {
        fputs("Common V2 mapping/planning requires --path=/absolute/file\n",stderr);
        return 2;
    }
    const uint64_t required=recovering?IFS_FS_DEFRAG_RECOVER:
        analysing?IFS_FS_DEFRAG_MAP:
        IFS_FS_DEFRAG_PLAN|IFS_FS_DEFRAG_EXECUTE|
        IFS_FS_DEFRAG_VERIFY|IFS_FS_DEFRAG_RECOVER;
    if((defrag->header.capabilities & required)!=required) {
        fputs("Provider does not qualify the requested strict V2 operation\n",stderr);
        return 2;
    }
    const int writable=!analysing;
    CommonV2Host b={0};
    b.services=services;
    if(services->open_target(services->context,command->argv[2],writable,
                             NULL,0u,&b.target)) {
        fputs("Cannot obtain trusted exclusive V2 storage lease\n",stderr);
        return 1;
    }
    int result=1;
    Target *t=b.target.handle;
    uint32_t logical=512u,physical=512u;
    if(t && t->device.is_block) {
        int logical_kernel=0,physical_kernel=0;
        if(ioctl(t->device.fd,BLKSSZGET,&logical_kernel)!=0 ||
           logical_kernel<512 || logical_kernel>65536 ||
           (logical_kernel & (logical_kernel-1)) ||
           (requested_sector && requested_sector!=(uint32_t)logical_kernel))
            goto cleanup_target;
        logical=(uint32_t)logical_kernel;
        if(ioctl(t->device.fd,BLKPBSZGET,&physical_kernel)==0 &&
           physical_kernel>=logical_kernel && physical_kernel<=65536 &&
           !(physical_kernel & (physical_kernel-1)))
            physical=(uint32_t)physical_kernel;
        else physical=logical;
    } else if(requested_sector) logical=physical=requested_sector;
    IfsFsHostRuntimeV2 h={0};
    h.abi_version=IFS_FS_HOST_ABI_V2;
    h.structure_bytes=sizeof(h);
    h.capabilities=(writable?
         IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE:
         IFS_FS_HOST_READ_ONLY)|IFS_FS_HOST_CANCEL;
    h.volume_bytes=b.target.bytes;
    h.logical_sector_bytes=logical;
    h.physical_sector_bytes=physical;
    h.maximum_transfer_bytes=65536u;
    h.context=&b;
    h.read_at=v2_read_at;
    h.write_at=writable?v2_write_at:NULL;
    h.flush=writable?v2_sync:NULL;
    h.allocate=v2_allocate;
    h.release=v2_release;
    h.cancelled=v2_cancelled;
    if(!ifs_fs_host_v2_valid(&h))goto cleanup_target;
    uint32_t variant=0u;
    if(core->probe(&h,&variant)!=IFS_FS_OK) {
        fputs("V2 provider rejected the target's filesystem geometry\n",stderr);
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
    if(recovering) {
        uint64_t repaired=0u;
        operation_ok=defrag->recover(volume,0u,&repaired)==IFS_FS_OK;

    } else if(analysing) {
        IfsFsExtentV2 extents[32]={{0}};
        uint32_t written=0u;
        int rc=defrag->map(volume,path,0u,extents,32u,&written);
        operation_ok=rc==IFS_FS_OK && written<=32u;

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
    if(!result)fputs("Common V2 provider operation verified and durably synced\n",stdout);
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
    Command command={argc,argv,format};
    return ifs_filesystem_module_call(format,filename,
        IFS_FS_COMMON_EXPORT_V2,run_common_v2,&command);
}

/* OFS.so's existing native Defrag V2 operation is deliberately separate
 * from strict Common V2 discovery and execution. This is not a fallback:
 * only the explicitly named OFS worker may request this entry point.
 * Remove this path when OFS.so exposes qualified Common V2 DEFRAG. */
typedef struct {
    int argc;
    char **argv;
} OfsNativeCommand;

static int run_ofs_native(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    const OfsNativeCommand *command = opaque;
    const IfsDefragModuleV2 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "Defrag entry width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsDefragModuleV2 *api = entry ? entry() : NULL;
    if (!api || api->abi_version != IFS_DEFRAG_ABI_V2 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, "ofs") != 0 || !api->run ||
        command->argc < 2 || !command->argv[1]) {
        fputs("OFS.so does not expose its expected native Defrag interface\n", stderr);
        return 1;
    }
    const char *operation = command->argv[1];
    uint64_t required = 0u;
    if (!strcmp(operation, "defrag"))
        required = IFS_DEFRAG_ANALYSE | IFS_DEFRAG_RELAYOUT |
                   IFS_DEFRAG_RECOVER;
    else if (!strcmp(operation, "growth-defrag"))
        required = IFS_DEFRAG_ANALYSE | IFS_DEFRAG_GROWTH |
                   IFS_DEFRAG_RECOVER;
    else if (!strcmp(operation, "recover"))
        required = IFS_DEFRAG_RECOVER;
    else if (!strcmp(operation, "analyse") ||
             !strcmp(operation, "analyse-json") ||
             !strcmp(operation, "map"))
        required = IFS_DEFRAG_ANALYSE;
    if (required == 0u || (api->capabilities & required) != required) {
        fputs("OFS.so does not qualify the requested native operation\n", stderr);
        return 1;
    }
    return api->run(host, command->argc, command->argv);
}

int ifs_filesystem_module_run_ofs_native(int argc, char **argv) {
    if (argc < 3 || !argv || !argv[1] || !argv[2])
        return 2;
    if (!strcmp(argv[1], "defrag") ||
        !strcmp(argv[1], "growth-defrag") ||
        !strcmp(argv[1], "recover")) {
        ld_stop_clear();
        ld_stop_install_handlers();
        ld_stop_report_ready();
    }
    OfsNativeCommand command = {argc, argv};
    return ifs_filesystem_module_call("ofs", "OFS.so",
        "infiltrator_filesystem_defrag_v2", run_ofs_native, &command);
}
