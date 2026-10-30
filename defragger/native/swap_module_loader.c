/* SPDX-License-Identifier: GPL-3.0-or-later
 * The SWAP worker is now a format-neutral loader only. SWAP-specific parsing,
 * map generation and header interpretation are implemented in SWAP.so.
 */
#include "filesystem_module_host.h"
#include "ifs_swap_module_v1.h"
#include <stdio.h>
#include <string.h>
typedef struct { int argc; char **argv; } Request;
static int run_swap(const IfsDefragHostV2 *host, void *symbol, void *user) {
    (void)host;
    const IfsSwapModuleV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "dlsym pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsSwapModuleV1 *api = entry ? entry() : NULL;
    if (!api || api->abi_version != IFS_SWAP_MODULE_V1 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, "swap") ||
        (api->capabilities & IFS_SWAP_CAP_RUN) == 0 || !api->run) {
        fputs("SWAP.so has no valid swap worker interface\n", stderr);
        return 1;
    }
    const Request *r = user;
    return api->run(r->argc, r->argv);
}
int main(int argc, char **argv) {
    Request request = { argc, argv };
    return ifs_filesystem_module_call("swap", "SWAP.so",
                "infiltrator_filesystem_swap_v1", run_swap, &request);
}
