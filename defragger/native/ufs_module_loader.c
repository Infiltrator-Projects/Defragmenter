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
    /* The V1 command "map --cells N" is a whole-volume allocation grid,
     * not the V2 DEFRAG_MAP per-file extent API. Preserve the original
     * grid/transaction paths; only an explicit --path file-map request can
     * select V2. Never reinterpret those older request arguments as extents,
     * and never fall back to V1 after selecting strict Common V2. */
    int file_map = 0;
    if (strcmp(argv[1], "map") == 0) {
        for (int i = 3; i < argc; ++i) {
            if (!argv[i]) return 2;
            if (!strcmp(argv[i], "--path") ||
                !strncmp(argv[i], "--path=", 7u)) {
                file_map = 1;
                break;
            }
        }
    }
    if (file_map || strcmp(argv[1], "volume-info") == 0) {
        char format[16] = {0};
        char filename[64] = {0};
        if (ifs_filesystem_module_detect_ufs_v2(
                request.target, format, sizeof(format),
                filename, sizeof(filename)) != 0) {
            fputs("No unique trusted UFS Common V2 provider recognises the target\n",
                  stderr);
            return 1;
        }
        return ifs_filesystem_module_run(format, filename, argc, argv);
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
