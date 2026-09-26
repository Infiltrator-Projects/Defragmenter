// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LDTM_RESERVED_CAPTURE 65536U

static int run_command(const char *const argv[]) {
    pid_t child;
    int status = 0;
    char program[PATH_MAX];
    if (argv == NULL || argv[0] == NULL ||
        ldtm_resolve_program(argv[0], program, sizeof(program)) != 0)
        return -1;
    child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execv(program, (char *const *)argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int capture_command(const char *const argv[], char *output, size_t capacity) {
    int pipefd[2];
    pid_t child;
    int status = 0;
    size_t used = 0U;
    char program[PATH_MAX];
    if (argv == NULL || argv[0] == NULL || output == NULL || capacity < 2U ||
        ldtm_resolve_program(argv[0], program, sizeof(program)) != 0)
        return -1;
    if (pipe(pipefd) != 0) return -1;
    child = fork();
    if (child < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        (void)close(pipefd[1]);
        execv(program, (char *const *)argv);
        _exit(127);
    }
    (void)close(pipefd[1]);
    while (used + 1U < capacity) {
        ssize_t got = read(pipefd[0], output + used, capacity - used - 1U);
        if (got < 0) {
            if (errno == EINTR) continue;
            (void)close(pipefd[0]);
            (void)waitpid(child, &status, 0);
            return -1;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    (void)close(pipefd[0]);
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    output[used] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return used + 1U < capacity ? 0 : -1;
}

static const LdtmFilesystemSpec *reserved_spec_for_label(const char *label) {
    size_t index;
    if (label == NULL) return NULL;
    for (index = 0U; index < ldtm_spec_count(); ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        if (spec->creator == LDTM_CREATOR_MANUAL && strcmp(spec->label, label) == 0)
            return spec;
    }
    return NULL;
}

int ldtm_is_reserved_partition_label(const char *label) {
    return reserved_spec_for_label(label) != NULL;
}

int ldtm_sanitize_reserved_partitions(const char *device) {
    const char *const lsblk_argv[] = {
        "lsblk", "-n", "-r", "-p", "-o", "PATH,PARTLABEL", "--", device, NULL
    };
    char *output;
    char *line;
    char *saveptr = NULL;
    size_t expected = 0U;
    size_t cleared = 0U;
    size_t index;

    if (device == NULL || *device == '\0' || geteuid() != 0) return -1;
    for (index = 0U; index < ldtm_spec_count(); ++index) {
        if (ldtm_specs()[index].creator == LDTM_CREATOR_MANUAL) ++expected;
    }
    output = malloc(LDTM_RESERVED_CAPTURE);
    if (output == NULL) return -1;
    if (capture_command(lsblk_argv, output, LDTM_RESERVED_CAPTURE) != 0) {
        free(output);
        return -1;
    }

    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        char path[PATH_MAX];
        char label[128];
        if (sscanf(line, "%4095s %127s", path, label) == 2) {
            const LdtmFilesystemSpec *spec = reserved_spec_for_label(label);
            if (spec != NULL) {
                const char *const wipe_argv[] = {
                    "wipefs", "--all", "--force", path, NULL
                };
                if (run_command(wipe_argv) != 0) {
                    fprintf(stderr, "Could not clear stale signatures from reserved partition %s (%s).\n",
                            path, label);
                    free(output);
                    return -1;
                }
                printf("LDTM_STATUS\t%s\treserved\treserved partition signatures cleared\n",
                       spec->key);
                fflush(stdout);
                ++cleared;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(output);

    if (cleared != expected) {
        fprintf(stderr, "Reserved-partition sanitation found %zu of %zu expected slots.\n",
                cleared, expected);
        return -1;
    }
    {
        const char *const partx_argv[] = {"partx", "-u", device, NULL};
        if (run_command(partx_argv) != 0) return -1;
    }
    if (ldtm_program_available("udevadm")) {
        const char *const udev_argv[] = {"udevadm", "settle", NULL};
        if (run_command(udev_argv) != 0) return -1;
    }
    puts("Reserved Test Media partitions are signature-clean.");
    fflush(stdout);
    return 0;
}
