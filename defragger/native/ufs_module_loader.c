/* SPDX-License-Identifier: GPL-3.0-or-later
 * The Defragmenter host never interprets UFS disk structures.
 * UFS1.so and UFS2.so independently recognise and mutate only their own
 * on-disk formats. First probe both modules, then dispatch to the owner.
 */
#include "filesystem_module_host.h"
#include "ifs_ufs_module_v1.h"
#include "ifs_ufs_probe_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    int argc;
    char **argv;
    const char *format;
    const char *target;
} Request;

static int probe_volume(const IfsDefragHostV2 *host, void *symbol, void *opaque)
{
    (void)host;
    const IfsUfsProbeV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "function pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsUfsProbeV1 *api = entry ? entry() : NULL;
    const Request *r = opaque;
    if (!r || !r->target || !api ||
        api->abi_version != IFS_UFS_PROBE_ABI_V1 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, r->format) || !api->accepts_volume)
        return 1;
    return api->accepts_volume(r->target) == 0 ? 0 : 1;
}

static int execute(const IfsDefragHostV2 *host, void *symbol, void *opaque)
{
    (void)host;
    const IfsUfsModuleV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "function pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsUfsModuleV1 *api = entry ? entry() : NULL;
    const Request *r = opaque;
    if (!r || !api || api->abi_version != IFS_UFS_MODULE_V1 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, r->format) ||
        (api->capabilities & IFS_UFS_CAP_RUN) == 0 || !api->run) {
        fputs("Incompatible or missing UFS1/UFS2 filesystem module interface\n", stderr);
        return 1;
    }
    return api->run(r->argc, r->argv, ifs_defrag_services_v1());
}

static int use_driver(const char *format, const char *filename, Request *request)
{
    request->format = format;
    return ifs_filesystem_module_call(
        format, filename, "infiltrator_filesystem_ufs_v1",
        execute, request);
}

static int detect_driver(const char *format, const char *filename, Request *request)
{
    request->format = format;
    return ifs_filesystem_module_call(
        format, filename, "infiltrator_filesystem_ufs_probe_v1",
        probe_volume, request);
}

int main(int argc, char **argv)
{
    Request request = { argc, argv, NULL, argc >= 3 ? argv[2] : NULL };
    if (argc == 2 && strcmp(argv[1], "--version") == 0)
        return use_driver("ufs2", "UFS2.so", &request);
    if (!request.target) {
        fputs("Specify the UFS target device or image\n", stderr);
        return 2;
    }
    /* Always probe read-only before allowing either engine to mutate media.
     * Probe failures never trigger a filesystem-specific local fallback.
     */
    if (detect_driver("ufs2", "UFS2.so", &request) == 0)
        return use_driver("ufs2", "UFS2.so", &request);
    if (detect_driver("ufs1", "UFS1.so", &request) == 0)
        return use_driver("ufs1", "UFS1.so", &request);
    fputs("Neither trusted UFS1.so nor UFS2.so recognises the target; operation refused\n", stderr);
    return 1;
}
