/* SPDX-License-Identifier: GPL-3.0-or-later
 * SWAP operations are dispatched through the sole Common V2 ABI.
 * Defragmenter owns trusted module discovery and the exclusive target lease;
 * SWAP.so owns all swap metadata and geometry interpretation.
 * A V2 CORE-only provider must never be mistaken for a qualified DEFRAG
 * writer: the generic host rejects unsupported requests before writing.
 */
#include "filesystem_module_host.h"
#include "infiltratr/filesystem_module_v2.h"
#include <stdio.h>
#include <string.h>

static int identify_swap(const IfsDefragHostV2 *host,
                         void *symbol, void *opaque)
{
    IfsFsModuleApiEntryV2 entry = NULL;
    const IfsFsModuleApiV2 *api;
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *volume;
    (void)host;
    (void)opaque;
    _Static_assert(sizeof(entry) == sizeof(symbol),
                   "dynamic symbol pointer width");
    memcpy(&entry, &symbol, sizeof(entry));
    api = entry ? entry() : NULL;
    if (!ifs_fs_api_v2_valid(api, "swap") ||
        !(api->runtime_profiles & IFS_FS_PROFILE_HOSTED))
        return 1;
    core = (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(
        api, IFS_FS_GROUP_CORE);
    volume = (const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(
        api, IFS_FS_GROUP_VOLUME);
    if (!core || !core->probe ||
        !(core->header.capabilities & IFS_FS_CORE_PROBE) ||
        !volume || !volume->format ||
        !(volume->header.capabilities & IFS_FS_VOLUME_FORMAT))
        return 1;
    puts("defragmenter-swap-worker: strict Common V2 SWAP.so verified");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--version"))
        return ifs_filesystem_module_call(
            "swap", "SWAP.so", IFS_FS_COMMON_EXPORT_V2,
            identify_swap, NULL);

    /* The installed SWAP provider currently advertises probe and format,
     * not a mount/DEFRAG map. Honour actual capabilities: the generic
     * execution path refuses unqualified relocation or false analysis. */
    return ifs_filesystem_module_run("swap", "SWAP.so", argc, argv);
}
