// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "version.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [--version]\n"
            "       %s --worker prepare DEVICE --confirmed DEVICE --fingerprint SHA256\n"
            "       %s --worker verify DEVICE\n",
            program, program, program);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("Defragmenter Test Media %s\n", LD_VERSION);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--worker") == 0) {
        if (argc == 8 && strcmp(argv[2], "prepare") == 0 &&
            strcmp(argv[4], "--confirmed") == 0 &&
            strcmp(argv[6], "--fingerprint") == 0) {
            int result = ldtm_worker_prepare(argv[3], argv[5], argv[7]);
            if (result != 0) return result;
            if (ldtm_sanitize_reserved_partitions(argv[3]) != 0) {
                fputs("Test Media build completed, but reserved partition sanitation failed.\n",
                      stderr);
                return 2;
            }
            return 0;
        }
        if (argc == 4 && strcmp(argv[2], "verify") == 0) {
            return ldtm_worker_verify(argv[3]);
        }
        usage(argv[0]);
        return 2;
    }
    gtk_init(&argc, &argv);
    ldtm_apply_mb_theme();
    return ldtm_gui_main(argc, argv);
}
