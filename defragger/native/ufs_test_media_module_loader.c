/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS test-media command host. UFS on-disk formatting and file population
 * live exclusively in UFS.so; no local UFS disk writer is linked here.
 */
#include "filesystem_module_host.h"
#include "ifs_ufs_module_v1.h"
#include <stdio.h>
#include <string.h>
typedef struct { int argc; char **argv; } Request;
static int run_format(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    (void)host;
    const IfsUfsModuleV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "function pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsUfsModuleV1 *api = entry ? entry() : NULL;
    if (!api || api->abi_version != IFS_UFS_MODULE_V1 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, "ufs") != 0 ||
        (api->capabilities & IFS_UFS_CAP_TEST_MEDIA) == 0 ||
        !api->format_test_media) {
        fputs("UFS.so lacks the required Test Media formatter\n", stderr);
        return 1;
    }
    const Request *request = opaque;
    return api->format_test_media(request->argc, request->argv);
}
int main(int argc, char **argv) {
    Request request = { argc, argv };
    return ifs_filesystem_module_call("ufs", "UFS.so",
        "infiltrator_filesystem_ufs_v1", run_format, &request);
}
