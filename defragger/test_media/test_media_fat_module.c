/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test Media owns the deterministic corpus and interleaving workload. The
 * installed FAT module owns formatting, directory records and allocation.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "fat_module_abi.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const LdtmFilesystemSpec *spec;
    const char *path;
    const IfsDefragHostV2 *host;
    IfsDefragTarget target;
} FatFixture;

static int read_io(void *opaque, uint64_t at, void *p, uint32_t n) {
    FatFixture *f = opaque;
    return f->host->read_at(f->host->context, &f->target, at, p, n);
}
static int write_io(void *opaque, uint64_t at, const void *p, uint32_t n) {
    FatFixture *f = opaque;
    return f->host->write_at(f->host->context, &f->target, at, p, n);
}
static int flush_io(void *opaque) {
    FatFixture *f = opaque;
    return f->host->sync_target(f->host->context, &f->target);
}
static uint32_t timestamp_io(void *opaque) {
    (void)opaque;
    return (20U << 25U) | (1U << 21U) | (1U << 16U); /* 2000-01-01 */
}
/* Verification holds a read-only host lease. A zero clock suppresses FAT
 * access-date updates in V3.read; otherwise reads would try writing metadata. */
static uint32_t timestamp_verify_io(void *opaque) {
    (void)opaque;
    return 0U;
}
static const IfsFatfsModuleV4 *interface(void *symbol, const LdtmFilesystemSpec *spec) {
    const IfsFatfsModuleV4 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "module symbol width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsFatfsModuleV4 *api = entry ? entry() : NULL;
    const uint64_t required = IFS_FATFS_CAP_FORMAT;
    if (!api || api->abi_version != IFS_FATFS_MODULE_ABI_V4 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, spec->key) || (api->capabilities & required) != required ||
        !api->format || !api->v3) return NULL;
    const IfsFatfsModuleV3 *v3 = api->v3;
    return v3->abi_version == IFS_FATFS_MODULE_ABI && v3->structure_bytes >= sizeof(*v3) &&
        (v3->capabilities & (IFS_FATFS_CAP_CREATE | IFS_FATFS_CAP_WRITE_EXISTING)) ==
            (IFS_FATFS_CAP_CREATE | IFS_FATFS_CAP_WRITE_EXISTING) &&
        v3->format_id && !strcmp(v3->format_id, spec->key) && v3->mount &&
        v3->unmount && v3->create && v3->write ? api : NULL;
}
static int available(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    (void)host;
    FatFixture *f = opaque;
    return interface(symbol, f->spec) ? 0 : -1;
}
static int create_fixture(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    FatFixture *f = opaque;
    const IfsFatfsModuleV4 *api = interface(symbol, f->spec);
    if (!api) return -1;
    f->host = host;
    if (host->open_target(host->context, f->path, 1, NULL, 0, &f->target)) return -1;
    /* A 2-GiB FAT32 fixture with 16-KiB clusters stays >65,525 clusters;
     * use this volume-valid geometry to avoid 4-KiB-per-file-sync overhead. */
    const uint32_t unit = f->spec->creator == LDTM_CREATOR_FAT32 ? 16384U : 65536U;
    const IfsFatfsIo io = {f, f->target.bytes, 512U, read_io, write_io, flush_io, timestamp_io};
    int result = -1;
    if (f->target.bytes != (uint64_t)f->spec->size_mib * LDTM_MIB ||
        host->stop_requested(host->context)) goto done;
    /* Clear only filesystem-neutral stale signatures while the validated
     * module and target descriptor are both held. A module replacement between
     * readiness and dispatch must not erase media before ABI validation. */
    uint8_t buffer[65536] = {0};
    for (uint64_t at = 0U; at < LDTM_MIB; at += sizeof(buffer)) {
        if (host->stop_requested(host->context) ||
            write_io(f, at, buffer, sizeof(buffer)) ||
            write_io(f, f->target.bytes - LDTM_MIB + at, buffer, sizeof(buffer))) goto done;
    }
    if (api->format(&io, unit) || api->v3->mount(&io)) goto done;
    const IfsFatfsModuleV3 *v3 = api->v3;
    char name[16];
    result = 0;
    for (unsigned file = 0U; !result && file < LDTM_RAW_FILES; ++file) {
        ldtm_raw_name(file, name);
        result = v3->create(name);
    }
    /* Append the first half of each file, then the second half. Interleaving
     * genuine module allocations creates fragmentation without editing a FAT
     * entry or depending on the module's particular allocation algorithm. */
    for (unsigned phase = 0U; !result && phase < 2U; ++phase) {
        for (unsigned file = 0U; !result && file < LDTM_RAW_FILES; ++file) {
            const uint64_t bytes = ldtm_raw_file_bytes(f->spec, file);
            if (bytes > UINT32_MAX) { result = -1; break; }
            const uint64_t units = (bytes + unit - 1U) / unit;
            uint64_t split = (units > 1U ? units / 2U : units) * unit;
            if (split > bytes) split = bytes;
            uint64_t at = phase == 0U ? 0U : split;
            const uint64_t end = phase == 0U ? split : bytes;
            ldtm_raw_name(file, name);
            while (!result && at < end) {
                const uint32_t n = end - at < sizeof(buffer) ? (uint32_t)(end - at) : sizeof(buffer);
                if (host->stop_requested(host->context)) { result = -1; break; }
                ldtm_raw_payload(buffer, n, file, at);
                result = v3->write(name, (uint32_t)at, buffer, n);
                at += n;
            }
        }
    }
    const int unmounted = v3->unmount();
    if (!result) result = unmounted;
done:
    host->close_target(host->context, &f->target);
    return result;
}
static int call_module(FatFixture *f, IfsFilesystemCall call) {
    const unsigned bits = f->spec->creator == LDTM_CREATOR_FAT12 ? 12U :
                          f->spec->creator == LDTM_CREATOR_FAT16 ? 16U :
                          f->spec->creator == LDTM_CREATOR_FAT32 ? 32U : 0U;
    if (!bits) return -1;
    char module[16];
    (void)snprintf(module, sizeof(module), "FAT%u.so", bits);
    return ifs_filesystem_module_call(f->spec->key, module,
        "infiltrator_filesystem_module_v4", call, f);
}
int ldtm_fat_module_available(const LdtmFilesystemSpec *spec) {
    FatFixture f = {.spec = spec};
    return spec && call_module(&f, available) == 0;
}
int ldtm_fat_module_create(const LdtmFilesystemSpec *spec, const char *path) {
    FatFixture f = {.spec = spec, .path = path};
    return spec && path ? call_module(&f, create_fixture) : -1;
}

/* Verification must follow the external module's read, stat, and directory
 * APIs. No FAT boot sector, chain, or directory-entry parsing is permitted here.
 * A read-only host lease ensures verification can never mutate its fixture.
 */
static int verify_fixture(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    FatFixture *f = opaque;
    const IfsFatfsModuleV4 *api = interface(symbol, f->spec);
    if (!api || !api->v3) return -1;
    const IfsFatfsModuleV3 *v3 = api->v3;
    if ((v3->capabilities & (IFS_FATFS_CAP_READ | IFS_FATFS_CAP_DIRECTORY)) !=
        (IFS_FATFS_CAP_READ | IFS_FATFS_CAP_DIRECTORY) ||
        !v3->read || !v3->stat || !v3->directory_entry) return -1;
    f->host = host;
    if (host->open_target(host->context, f->path, 0, NULL, 0, &f->target))
        return -1;
    const IfsFatfsIo io = { f, f->target.bytes, 512U,
                            read_io, write_io, flush_io, timestamp_verify_io };
    int result = -1;
    int mounted = 0;
    if (f->target.bytes != (uint64_t)f->spec->size_mib * LDTM_MIB ||
        v3->mount(&io) != 0) goto done;
    mounted = 1;
    uint8_t actual[65536], expected[65536];
    bool seen[LDTM_RAW_FILES] = {false};
    for (uint32_t index = 0; index <= LDTM_RAW_FILES; ++index) {
        IfsFatfsEntry entry = {0};
        const int status = v3->directory_entry("/", index, &entry);
        if (index == LDTM_RAW_FILES) {
            if (status != 1) goto done; /* No unexpected files. */
            break;
        }
        if (status != 0) goto done;
        int file = ldtm_raw_name_index(entry.name);
        if (file < 0 || (unsigned)file >= LDTM_RAW_FILES ||
            seen[file] || (entry.attributes & 0x10U) != 0U ||
            (uint64_t)entry.bytes != ldtm_raw_file_bytes(f->spec, (unsigned)file))
            goto done;
        seen[file] = true;
    }
    for (unsigned file = 0; file < LDTM_RAW_FILES; ++file) {
        if (!seen[file]) goto done;
        char name[16]; ldtm_raw_name(file, name);
        IfsFatfsEntry stat = {0};
        const uint64_t length = ldtm_raw_file_bytes(f->spec, file);
        if (length > UINT32_MAX || v3->stat(name, &stat) != 0 ||
            stat.bytes != length || (stat.attributes & 0x10U) != 0U)
            goto done;
        for (uint64_t offset = 0; offset < length; ) {
            const uint32_t count = length - offset < sizeof(actual)
                                 ? (uint32_t)(length - offset) : sizeof(actual);
            if (host->stop_requested(host->context) ||
                v3->read(name, (uint32_t)offset, actual, count) != 0)
                goto done;
            ldtm_raw_payload(expected, count, file, offset);
            if (memcmp(actual, expected, count) != 0) goto done;
            offset += count;
        }
    }
    result = 0;
done:
    if (mounted && v3->unmount() != 0) result = -1;
    host->close_target(host->context, &f->target);
    return result;
}
int ldtm_raw_fat(const LdtmFilesystemSpec *spec, const char *path, bool create) {
    if (!spec || !path) return -1;
    if (create) return ldtm_fat_module_create(spec, path);
    FatFixture f = { .spec = spec, .path = path };
    return call_module(&f, verify_fixture);
}
