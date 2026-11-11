/* SPDX-License-Identifier: GPL-3.0-or-later
 * SWAP Test Media talks directly to the source-owned SWAP.so Common V2 API.
 * The host supplies checked, exclusive target I/O only; the provider owns
 * formatting, signature/geometry validation and all filesystem semantics.
 * No retired per-format V1 export or compatibility fallback is permitted.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "infiltratr/filesystem_module_v2.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SWAP_TEST_PAGE_BYTES = 4096u };

typedef struct {
    const LdtmFilesystemSpec *spec;
    const char *path;
    int create;
    const IfsDefragHostV2 *host;
    IfsDefragTarget target;
} SwapFixture;

static int swap_read(void *context, uint64_t at, void *out, uint32_t bytes)
{
    SwapFixture *f = context;
    return f->host->read_at(f->host->context, &f->target,
                            at, out, bytes) == 0 ?
           IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int swap_write(void *context, uint64_t at,
                      const void *data, uint32_t bytes)
{
    SwapFixture *f = context;
    return f->host->write_at(f->host->context, &f->target,
                             at, data, bytes) == 0 ?
           IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int swap_flush(void *context)
{
    SwapFixture *f = context;
    return f->host->sync_target(f->host->context,
                                &f->target) == 0 ?
           IFS_FS_OK : IFS_FS_IO_ERROR;
}
static void *swap_allocate(void *context, size_t bytes)
{
    (void)context;
    return malloc(bytes);
}
static void swap_release(void *context, void *memory)
{
    (void)context;
    free(memory);
}
static IfsFsHostRuntimeV2 swap_host(SwapFixture *f)
{
    IfsFsHostRuntimeV2 io = {0};
    io.abi_version = IFS_FS_HOST_ABI_V2;
    io.structure_bytes = sizeof(io);
    io.capabilities = f->create ?
        IFS_FS_HOST_EXCLUSIVE | IFS_FS_HOST_DURABLE :
        IFS_FS_HOST_READ_ONLY;
    io.context = f;
    io.volume_bytes = f->target.bytes;
    io.logical_sector_bytes = 512u;
    io.physical_sector_bytes = 512u;
    io.maximum_transfer_bytes = 65536u;
    io.read_at = swap_read;
    io.write_at = f->create ? swap_write : NULL;
    io.flush = f->create ? swap_flush : NULL;
    io.allocate = swap_allocate;
    io.release = swap_release;
    return io;
}

/* A strict V2 CORE probe and VOLUME formatter are the only capabilities
 * needed here. In particular, SWAP has no mount, file or DEFRAG operation. */
static const IfsFsModuleApiV2 *swap_module(void *symbol)
{
    IfsFsModuleApiEntryV2 entry = NULL;
    const IfsFsModuleApiV2 *api;
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *volume;
    _Static_assert(sizeof(entry) == sizeof(symbol),
                   "dynamic function pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    api = entry ? entry() : NULL;
    if (!ifs_fs_api_v2_valid(api, "swap"))
        return NULL;
    core = (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(
        api, IFS_FS_GROUP_CORE);
    volume = (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(
        api, IFS_FS_GROUP_VOLUME);
    if (!core || !volume ||
        !(core->header.capabilities & IFS_FS_CORE_PROBE) ||
        !(volume->header.capabilities & IFS_FS_VOLUME_FORMAT) ||
        !core->probe || !volume->format)
        return NULL;
    return api;
}
static int swap_available(const IfsDefragHostV2 *host,
                          void *symbol, void *opaque)
{
    (void)host;
    (void)opaque;
    return swap_module(symbol) ? 0 : 1;
}
static int swap_module_call(const IfsDefragHostV2 *host,
                            void *symbol, void *opaque)
{
    SwapFixture *f = opaque;
    const IfsFsModuleApiV2 *api = swap_module(symbol);
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *volume;
    IfsFsHostRuntimeV2 io;
    uint32_t variant = 0u;
    int status = 1;

    if (!api || !f || !f->spec || !f->path || !f->spec->label)
        return 1;
    core = (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(
        api, IFS_FS_GROUP_CORE);
    volume = (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(
        api, IFS_FS_GROUP_VOLUME);
    f->host = host;
    if (host->open_target(host->context, f->path, f->create,
                          NULL, 0, &f->target) != 0)
        return 1;

    if (f->target.bytes != (uint64_t)f->spec->size_mib * LDTM_MIB ||
        host->stop_requested(host->context))
        goto done;
    io = swap_host(f);
    if (!ifs_fs_host_v2_valid(&io))
        goto done;
    if (f->create &&
        volume->format(&io, SWAP_TEST_PAGE_BYTES,
                       f->spec->label, 0u) != IFS_FS_OK)
        goto done;
    /* Independently probe the newly formatted medium, or verify an existing
     * Test Media image, using the same authoritative provider and readonly
     * semantics for non-creation. */
    if (host->stop_requested(host->context) ||
        core->probe(&io, &variant) != IFS_FS_OK ||
        variant != SWAP_TEST_PAGE_BYTES)
        goto done;
    status = 0;
done:
    if (f->create && host->sync_target(host->context, &f->target) != 0)
        status = 1;
    host->close_target(host->context, &f->target);
    return status;
}
static int swap_call(SwapFixture *f, IfsFilesystemCall callback)
{
    return ifs_filesystem_module_call(
        "swap", "SWAP.so", IFS_FS_COMMON_EXPORT_V2, callback, f);
}
int ldtm_swap_module_available(const LdtmFilesystemSpec *spec)
{
    SwapFixture f = {0};
    if (!spec || spec->creator != LDTM_CREATOR_SWAP)
        return 0;
    f.spec = spec;
    return swap_call(&f, swap_available) == 0;
}
int ldtm_raw_swap(const LdtmFilesystemSpec *spec,
                  const char *path, bool create)
{
    SwapFixture f = {0};
    if (!spec || !path || spec->creator != LDTM_CREATOR_SWAP)
        return -1;
    f.spec = spec;
    f.path = path;
    f.create = create ? 1 : 0;
    return swap_call(&f, swap_module_call);
}
