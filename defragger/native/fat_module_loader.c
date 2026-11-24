/* SPDX-License-Identifier: GPL-3.0-or-later
 * FAT module routing uses the same trusted Common V2 filesystem host as every
 * other format. Filesystem identification comes from CORE.PROBE; allocation,
 * planning and movement are never implemented or reinterpreted here.
 */
#include "filesystem_module_host.h"
#include "version.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    static const char *const formats[] = {"fat12", "fat16", "fat32"};
    const char *selected_format = NULL;
    char selected_file[64] = {0};
    unsigned matches = 0u;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("defragmenter-fat-worker %s\n", LD_VERSION);
        return 0;
    }
    if (argc < 3 || !argv[1] || !argv[2]) {
        fputs("Usage: defragmenter-fat-worker COMMAND DEVICE [OPTIONS]\n", stderr);
        return 2;
    }

    /* Every identification read is performed through a trusted, verified
     * Common V2 .so with an exclusively read-only target lease. */
    for (size_t i = 0u; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        char candidate[64] = {0};
        char *probe_args[] = {argv[0], (char *)"probe-only", argv[2], NULL};
        if (ifs_filesystem_module_find_readonly_v2(formats[i], candidate,
                                                   sizeof(candidate)) != 0)
            continue;
        if (ifs_filesystem_module_run(formats[i], candidate, 3, probe_args) != 0)
            continue;
        if (++matches != 1u) {
            fputs("Ambiguous FAT Common V2 volume identity\n", stderr);
            return 1;
        }
        selected_format = formats[i];
        memcpy(selected_file, candidate, strlen(candidate) + 1u);
    }
    if (matches != 1u) {
        fputs("No trusted Common V2 FAT12/16/32 module matches the target\n",
              stderr);
        return 1;
    }
    if(!strcmp(argv[1],"identify")) {
        if(argc!=3)return 2;
        printf("{\"filesystem\":\"%s\"}\n",selected_format);
        return 0;
    }
    /* Mutations are admitted only when the selected module advertises real
     * V2 DEFRAG PLAN/EXECUTE/VERIFY callbacks. No legacy ABI or fallback. */
    return ifs_filesystem_module_run(selected_format, selected_file, argc, argv);
}
