/* SPDX-License-Identifier: GPL-3.0-or-later
 * SWAP Test Media uses SWAP.so. No filesystem header or bitmap is assembled
 * in Defragmenter. The host only provides bounded, checked raw I/O.
 */
#include "test_media_raw.h"
#include "filesystem_module_host.h"
#include "ifs_swap_module_v1.h"
#include <string.h>
typedef struct {
    const LdtmFilesystemSpec *spec;
    const char *path;
    int create;
    const IfsDefragHostV2 *host;
    IfsDefragTarget target;
} SwapFixture;
static int swap_read(void *p, uint64_t at, void *out, uint32_t n) {
    SwapFixture *f = p;
    return f->host->read_at(f->host->context, &f->target, at, out, n);
}
static int swap_write(void *p, uint64_t at, const void *in, uint32_t n) {
    SwapFixture *f = p;
    return f->host->write_at(f->host->context, &f->target, at, in, n);
}
static int swap_flush(void *p) {
    SwapFixture *f = p;
    return f->host->sync_target(f->host->context, &f->target);
}
static int swap_module_call(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    SwapFixture *f = opaque;
    const IfsSwapModuleV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym pointer mismatch");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsSwapModuleV1 *api = entry ? entry() : NULL;
    if (!api || api->abi_version != IFS_SWAP_MODULE_V1 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, "swap") ||
        (api->capabilities & (IFS_SWAP_CAP_FORMAT | IFS_SWAP_CAP_VERIFY)) !=
            (IFS_SWAP_CAP_FORMAT | IFS_SWAP_CAP_VERIFY) ||
        !api->format || !api->verify) return 1;
    f->host = host;
    if (host->open_target(host->context, f->path, f->create, NULL, 0, &f->target))
        return 1;
    int status = 1;
    if (f->target.bytes == (uint64_t)f->spec->size_mib * LDTM_MIB &&
        !host->stop_requested(host->context)) {
        IfsSwapIoV1 io = {f, f->target.bytes, swap_read, swap_write, swap_flush};
        status = f->create ? api->format(&io, f->spec->label) :
                             api->verify(&io);
    }
    host->close_target(host->context, &f->target);
    return status;
}
int ldtm_raw_swap(const LdtmFilesystemSpec *spec, const char *path, bool create) {
    if (!spec || !path || spec->creator != LDTM_CREATOR_SWAP) return -1;
    SwapFixture f = { .spec = spec, .path = path, .create = create ? 1 : 0 };
    return ifs_filesystem_module_call("swap", "SWAP.so",
               "infiltrator_filesystem_swap_v1", swap_module_call, &f);
}
