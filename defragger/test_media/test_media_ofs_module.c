/* SPDX-License-Identifier: GPL-3.0-or-later
 * The dataset belongs to Test Media; filesystem creation/allocation belongs to
 * OFS.so. Test Media calls Common V2 for formatting, writing and verification.
 */
#include "test_media.h"
#include "filesystem_module_host.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef struct {
    const char *path;
    const LdtmFragmentProfile *profile;
} Fixture;
static void payload(unsigned char *buffer, size_t length, uint32_t file, uint32_t block) {
    uint64_t state = UINT64_C(0x6a09e667f3bcc909) ^ ((uint64_t)file << 40) ^
                     ((uint64_t)block * UINT64_C(0x9e3779b97f4a7c15));
    for (size_t i = 0; i < length; ++i) {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        state *= UINT64_C(0x2545f4914f6cdd1d); buffer[i] = (unsigned char)(state >> 56);
    }
}
static int populate(const IfsFsCoreOpsV2 *core, void *session,
                    const IfsFsHostRuntimeV2 *io, const LdtmFragmentProfile *p) {
    if (!p || !p->files || p->files > 8 || !p->chunks || !p->chunk_kib) return -1;
    const char *base = "/Defragmenter-TestData";
    const char *directory = "/Defragmenter-TestData/fragmented-directory";
    const char *files = "/Defragmenter-TestData/fragmented-files";
    if (core->create(session, base, IFS_FS_ENTRY_DIRECTORY) ||
        core->create(session, files, IFS_FS_ENTRY_DIRECTORY) ||
        core->create(session, directory, IFS_FS_ENTRY_DIRECTORY)) return -1;
    char path[160];
    for (uint32_t i = 0; i < p->directory_initial; ++i) {
        (void)snprintf(path, sizeof(path), "%s/entry-%05u.txt", directory, i);
        if (io->cancelled(io->context) || core->create(session, path, IFS_FS_ENTRY_FILE))
            return -1;
        if ((i+1u)%512u==0u || i+1u==p->directory_initial) {
            (void)printf("OFS fixture: created %u/%u initial directory entries\n",
                         i+1u,p->directory_initial);
            (void)fflush(stdout);
        }
    }
    for (uint32_t i = 0; i < p->directory_initial; i += 2) {
        (void)snprintf(path, sizeof(path), "%s/entry-%05u.txt", directory, i);
        if (io->cancelled(io->context) || core->remove(session, path)) return -1;
        if ((i/2u+1u)%512u==0u || i+2u>=p->directory_initial) {
            (void)printf("OFS fixture: deleted %u/%u temporary entries\n",
                         i/2u+1u,(p->directory_initial+1u)/2u);
            (void)fflush(stdout);
        }
    }
    for (uint32_t i = 0; i < p->directory_second; ++i) {
        (void)snprintf(path, sizeof(path), "%s/entry-%05u.txt", directory, p->directory_initial + i);
        if (io->cancelled(io->context) || core->create(session, path, IFS_FS_ENTRY_FILE))
            return -1;
        if ((i+1u)%512u==0u || i+1u==p->directory_second) {
            (void)printf("OFS fixture: created %u/%u second-wave entries\n",
                         i+1u,p->directory_second);
            (void)fflush(stdout);
        }
    }
    uint32_t done[8] = {0};
    for (uint32_t i = 0; i < p->files; ++i) {
        (void)snprintf(path, sizeof(path), "%s/fragmented-%02u.bin", files, i);
        if (core->create(session, path, IFS_FS_ENTRY_FILE)) return -1;
    }
    unsigned char *buffer = io->allocate(io->context, 488U * 2048U);
    if (!buffer) return -1;
    int result = 0;
    for (uint32_t chunk = 0; !result && chunk < p->chunks; ++chunk) {
        (void)printf("OFS fixture: fragmented corpus round %u/%u\n",
                     chunk+1u,p->chunks);
        (void)fflush(stdout);
        for (uint32_t i = 0; !result && i < p->files; ++i) {
            uint64_t bytes = ldtm_profile_file_bytes(p, i);
            uint32_t chunks = ldtm_profile_file_chunks(p, i);
            if (bytes > UINT32_MAX || chunks < 2) { result = -1; break; }
            if (chunk >= chunks) continue;
            uint32_t blocks = (uint32_t)((bytes + 487U) / 488U);
            uint32_t run = blocks / chunks + (chunk < blocks % chunks ? 1U : 0U);
            (void)snprintf(path, sizeof(path), "%s/fragmented-%02u.bin", files, i);
            while (!result && run) {
                uint32_t count = run > 2048U ? 2048U : run, n = 0;
                for (uint32_t j = 0; j < count; ++j) {
                    uint32_t at = (done[i] + j) * 488U;
                    uint32_t take = bytes - at < 488U ? (uint32_t)(bytes - at) : 488U;
                    payload(buffer + n, take, i, done[i] + j); n += take;
                }
                result = core->write(session, path, done[i] * UINT64_C(488), buffer, n);
                done[i] += count; run -= count;
                if (io->cancelled(io->context)) result = -1;
            }
        }
    }
    io->release(io->context, buffer);
    return result;
}
static int open_writable(const IfsDefragHostV2 *host, const char *path,
                         const char *operation, IfsDefragTarget *target) {
    if (host->open_target(host->context, path, 1, NULL, 0, target)) {
        const int saved_errno = errno;
        fprintf(stderr, "OFS Test Media %s: exclusive target open failed for %s (errno=%d: %s). "
                        "Check for desktop automounts and open users.\n",
                operation, path, saved_errno,
                saved_errno ? strerror(saved_errno) : "target identity or safety refusal");
        /* Only exclusive-open contention is eligible for the caller's
         * retry of the explicitly selected disposable partition. */
        return saved_errno == EBUSY || saved_errno == EWOULDBLOCK ? -EBUSY : -1;
    }
    return 0;
}
static const IfsFsModuleApiV2 *common_api(void *symbol) {
    IfsFsModuleApiEntryV2 entry = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "module symbol width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsFsModuleApiV2 *api = entry ? entry() : NULL;
    return ifs_fs_api_v2_valid(api, "ofs") &&
           (api->runtime_profiles & IFS_FS_PROFILE_HOSTED) ? api : NULL;
}
static int fixture_capabilities(const IfsFsModuleApiV2 *api) {
    if (!api) return 0;
    const IfsFsCoreOpsV2 *core =
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume =
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    const uint64_t required_core = IFS_FS_CORE_MOUNT | IFS_FS_CORE_STAT | IFS_FS_CORE_READ |
        IFS_FS_CORE_CREATE | IFS_FS_CORE_REMOVE | IFS_FS_CORE_WRITE;
    const uint64_t required_volume = IFS_FS_VOLUME_FORMAT | IFS_FS_VOLUME_CHECK |
        IFS_FS_VOLUME_SYNC;
    return core && (core->header.capabilities & required_core) == required_core &&
           volume && (volume->header.capabilities & required_volume) == required_volume;
}
static int common_available(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    (void)host; (void)opaque;
    return fixture_capabilities(common_api(symbol)) ? 0 : -1;
}
static int populate_fixture(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    const Fixture *f = opaque;
    const IfsFsModuleApiV2 *api = common_api(symbol);
    if (!fixture_capabilities(api)) return -1;
    const IfsFsCoreOpsV2 *core =
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume =
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    IfsFilesystemTargetV2 binding = {.services = host};
    int r = open_writable(host, f->path, "populate", &binding.target);
    if (r) return r;

    IfsFsHostRuntimeV2 io;
    IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
    void *session = NULL;
    r = IFS_FS_INVALID;
    if (!ifs_filesystem_target_runtime_v2(&binding, 1, LDTM_OFS_BLOCK_BYTES, &io) &&
        !io.cancelled(io.context)) {
        r = core->mount(&io, &session, &info);
        if (r == IFS_FS_OK && session) {
            r = info.variant == 0u && info.block_bytes == LDTM_OFS_BLOCK_BYTES ?
                populate(core, session, &io, f->profile) : IFS_FS_UNSUPPORTED;
            uint64_t errors = UINT64_MAX;
            if (r == IFS_FS_OK &&
                (volume->check(session, 0u, &errors) != IFS_FS_OK || errors != 0u))
                r = IFS_FS_CORRUPT;
            if (r == IFS_FS_OK) r = volume->sync(session);
            const int closed = core->unmount(session);
            if (r == IFS_FS_OK) r = closed;
            if (r == IFS_FS_OK && io.flush(io.context)) r = IFS_FS_IO_ERROR;
        } else if (r == IFS_FS_OK) {
            r = IFS_FS_INVALID;
        }
    }
    if (r)
        fprintf(stderr, "OFS Test Media populate: Common V2 returned status %d on %s.\n",
                r, f->path);
    host->close_target(host->context, &binding.target);
    return r;
}
int ldtm_ofs_module_available(void) {
    return ifs_filesystem_module_call("ofs", "OFS.so", IFS_FS_COMMON_EXPORT_V2,
                                      common_available, NULL) == 0;
}
typedef struct { const char *path, *label; } OfsFormatRequest;
static int format_ofs(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    const OfsFormatRequest *q = opaque;
    const IfsFsModuleApiV2 *api = common_api(symbol);
    if (!api) return -1;
    const IfsFsVolumeOpsV2 *volume =
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    if (!volume || !(volume->header.capabilities & IFS_FS_VOLUME_FORMAT)) return -1;
    IfsFilesystemTargetV2 binding = {.services = host};
    int r = open_writable(host, q->path, "format", &binding.target);
    if (r) return r;
    IfsFsHostRuntimeV2 io;
    /* The corpus uses DOS0 blocks with a 488-byte payload. The formatter
     * derives its block size from the real host logical sector size. */
    r = IFS_FS_INVALID;
    if (!ifs_filesystem_target_runtime_v2(&binding, 1, LDTM_OFS_BLOCK_BYTES, &io)) {
        if (!io.cancelled(io.context))
            r = volume->format(&io, 0u, q->label, 0u);
        if (r == IFS_FS_OK && io.flush(io.context)) r = IFS_FS_IO_ERROR;
    }
    host->close_target(host->context, &binding.target);
    if (r != IFS_FS_OK)
        fprintf(stderr, "OFS Test Media format: Common V2 returned status %d on %s.\n",
                r, q->path);
    return r == IFS_FS_OK ? 0 : -1;
}
int ldtm_format_ofs_module(const char *path, const char *label) {
    if (!path) return -1;
    OfsFormatRequest q = {.path = path, .label = label ? label : "LD_OFS"};
    return ifs_filesystem_module_call("ofs", "OFS.so", IFS_FS_COMMON_EXPORT_V2,
                                      format_ofs, &q);
}
int ldtm_populate_ofs_module(const char *path, const LdtmFragmentProfile *profile) {
    if (!path || !profile) return -1;
    Fixture f = {.path = path, .profile = profile};
    return ifs_filesystem_module_call("ofs", "OFS.so", IFS_FS_COMMON_EXPORT_V2,
                                      populate_fixture, &f);
}

/* Independently check the full canonical corpus via OFS.so's public file and
 * directory APIs; Test Media does not inspect OFS blocks or bitmaps itself. */
typedef struct {
    const char *path;
    const LdtmFragmentProfile *profile;
    char *detail;
    size_t detail_capacity;
} OfsCheckRequest;
static int verify_payload(const IfsFsHostRuntimeV2 *io, const IfsFsCoreOpsV2 *core,
                          void *session, const LdtmFragmentProfile *profile) {
    if (!profile->files || profile->files > LDTM_TARGET_FILE_COUNT) return -1;
    char path[160], name[128];
    IfsFsEntryV2 file = {.structure_bytes = sizeof(file),
                         .name = name, .name_capacity = sizeof(name)};
    unsigned char actual[488U*32U], expected[488U*32U];
    for (uint32_t i = 0; i < profile->files; ++i) {
        (void)snprintf(path, sizeof(path),
            "/Defragmenter-TestData/fragmented-files/fragmented-%02u.bin", i);
        uint64_t bytes = ldtm_profile_file_bytes(profile, i);
        if (bytes > UINT32_MAX || core->stat(session, path, &file) != IFS_FS_OK ||
            file.type != IFS_FS_ENTRY_FILE || file.size_bytes != bytes) return -1;
        for (uint64_t pos = 0; pos < bytes;) {
            const uint32_t n = bytes - pos < sizeof(actual)
                ? (uint32_t)(bytes - pos) : (uint32_t)sizeof(actual);
            if (io->cancelled(io->context) ||
                core->read(session, path, pos, actual, n) != IFS_FS_OK) return -1;
            for (uint32_t off = 0; off < n;) {
                uint32_t take = n - off < 488U ? n - off : 488U;
                payload(expected + off, take, i, (uint32_t)((pos + off) / 488U));
                off += take;
            }
            if (memcmp(actual, expected, n)) return -1;
            pos += n;
        }
    }
    for (uint32_t i = 0; i < profile->directory_initial + profile->directory_second; ++i) {
        (void)snprintf(path, sizeof(path),
            "/Defragmenter-TestData/fragmented-directory/entry-%05u.txt", i);
        const int found = core->stat(session, path, &file);
        const int present = i >= profile->directory_initial || (i & 1U);
        if (found != (present ? IFS_FS_OK : IFS_FS_NOT_FOUND) ||
            (present && (file.type != IFS_FS_ENTRY_FILE || file.size_bytes != 0u)))
            return -1;
    }
    return 0;
}
static int inspect_ofs(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    const OfsCheckRequest *q = opaque;
    const IfsFsModuleApiV2 *api = common_api(symbol);
    if (!api) return -1;
    const IfsFsCoreOpsV2 *core =
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume =
        (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    const uint64_t required = IFS_FS_CORE_MOUNT |
        (q->profile ? IFS_FS_CORE_STAT | IFS_FS_CORE_READ : 0u);
    if (!core || (core->header.capabilities & required) != required ||
        !volume || !(volume->header.capabilities & IFS_FS_VOLUME_CHECK)) return -1;
    IfsFilesystemTargetV2 binding = {.services = host};
    if (host->open_target(host->context, q->path, 0, NULL, 0, &binding.target)) return -1;
    IfsFsHostRuntimeV2 io;
    IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
    void *session = NULL;
    int rc = -1;
    if (ifs_filesystem_target_runtime_v2(&binding, 0, 0u, &io) ||
        core->mount(&io, &session, &info) != IFS_FS_OK || !session) goto close_target;
    uint64_t errors = UINT64_MAX;
    if (info.variant == 0u && info.block_bytes == LDTM_OFS_BLOCK_BYTES &&
        volume->check(session, 0u, &errors) == IFS_FS_OK && errors == 0u)
        rc = q->profile ? verify_payload(&io, core, session, q->profile) : 0;
    if (core->unmount(session) != IFS_FS_OK) rc = -1;
close_target:
    host->close_target(host->context, &binding.target);
    if (q->detail && q->detail_capacity)
        (void)snprintf(q->detail, q->detail_capacity, "%s",
            rc ? "OFS.so integrity, corpus or directory verification failed" :
                 "OFS.so verified all corpus bytes and directory entries");
    return rc;
}
int ldtm_validate_ofs_module(const char *path) {
    if (!path) return -1;
    OfsCheckRequest q = {.path = path};
    return ifs_filesystem_module_call("ofs", "OFS.so", IFS_FS_COMMON_EXPORT_V2,
                                      inspect_ofs, &q) == 0 ? 0 : -1;
}
int ldtm_verify_ofs_module(const char *path,const LdtmFragmentProfile *profile,
                           int after,char *detail,size_t capacity) {
    (void)after; /* Fragment layout is checked by the production module analyser. */
    if (!path || !profile) return -1;
    OfsCheckRequest q = {.path = path, .profile = profile,
                         .detail = detail, .detail_capacity = capacity};
    return ifs_filesystem_module_call("ofs", "OFS.so", IFS_FS_COMMON_EXPORT_V2,
                                      inspect_ofs, &q) == 0 ? 0 : -1;
}
