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
    const uint32_t unit = 65536U;
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
                          0U;
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
