/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "filesystem_module_host.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts(LD_VERSION); return 0; }
    if (argc < 3) { fputs("Usage: defragmenter-ofs-worker COMMAND DEVICE [OPTIONS]\n", stderr); return 2; }
    return ifs_filesystem_module_run("ofs", "OFS.so", argc, argv);
}
