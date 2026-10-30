/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The exFAT filesystem engine, allocation, journal and recovery live only in
 * exFAT.so. This process hosts the verified module; it does not implement exFAT. */
#include "filesystem_module_host.h"
#include "ifs_exfat_defrag_v1.h"
#include "ifs_defrag_host_services.h"
#include <stdio.h>
#include <string.h>
typedef struct { int argc; char **argv; } Request;
static int execute(const IfsDefragHostV2 *host, void *symbol, void *opaque) {
    (void)host;
    const IfsExfatDefragV1 *(*entry)(void) = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "function-pointer ABI size");
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsExfatDefragV1 *api = entry ? entry() : NULL;
    if (!api || api->abi_version != IFS_EXFAT_DEFRAG_ABI_V1 ||
        api->structure_bytes < sizeof(*api) || !api->format_id ||
        strcmp(api->format_id, "exfat") != 0 ||
        (api->capabilities & IFS_EXFAT_DEFRAG_CAP_RUN) != IFS_EXFAT_DEFRAG_CAP_RUN ||
        !api->run) {
        fputs("exFAT.so: missing or incompatible defragmentation interface\n", stderr);
        return 1;
    }
    const Request *request = opaque;
    return api->run(request->argc, request->argv, ifs_defrag_services_v1());
}
int main(int argc, char **argv) {
    Request request = {argc, argv};
    return ifs_filesystem_module_call("exfat", "exFAT.so",
        "infiltrator_filesystem_exfat_defrag_v1", execute, &request);
}
