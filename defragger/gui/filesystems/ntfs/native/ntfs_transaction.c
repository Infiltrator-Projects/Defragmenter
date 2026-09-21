// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_transaction.h"

#include "ntfs_native.h"

#include "ld_path.h"
#include "ld_runtime.h"

#include "infiltratr/config.h"
#include "infiltratr/core.h"
#include "infiltratr/posix.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTFS_JOURNAL_MAGIC "LINUX-DEFRAGGER-NTFS-JOURNAL-1"

static int ensure_directory_tree(const char *path, char **error)
{
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        ntfs_set_error(error, "cannot create NTFS journal directory %s: %s",
                       path, strerror(errno));
        return -1;
    }
    return 0;
}

static bool safe_journal_value(const char *value)
{
    return value != NULL && strchr(value, '\n') == NULL &&
           strchr(value, '\r') == NULL && strchr(value, '=') == NULL;
}

void ntfs_journal_free(NtfsJournal *state)
{
    if (state == NULL)
        return;
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    free(state->plan);
    memset(state, 0, sizeof(*state));
}

static bool journal_write_stream(FILE *file, const void *user_data)
{
    const NtfsJournal *state = user_data;
    (void)fprintf(file, "%s\n", NTFS_JOURNAL_MAGIC);
    (void)fprintf(file, "device=%s\n", state->device);
    (void)fprintf(file, "target_identity=%s\n", state->target_identity);
    (void)fprintf(file, "serial=%s\n", state->serial);
    (void)fprintf(file, "operation=%s\n", state->operation);
    (void)fprintf(file, "phase=%s\n", state->phase);
    (void)fprintf(file, "stage=%s\n", state->stage);
    (void)fprintf(file, "plan=%s\n", state->plan);
    (void)fprintf(file, "physical_bytes=%" PRIu64 "\n", state->physical_bytes);
    (void)fprintf(file, "filesystem_bytes=%" PRIu64 "\n", state->filesystem_bytes);
    (void)fprintf(file, "commit_cluster=%" PRIu64 "\n", state->commit_cluster);
    (void)fprintf(file, "move_clusters=%" PRIu64 "\n", state->move_clusters);
    (void)fprintf(file, "workspace_start=%" PRIu64 "\n", state->workspace_start);
    (void)fprintf(file, "workspace_clusters=%" PRIu64 "\n", state->workspace_clusters);
    return !ferror(file);
}

int ntfs_journal_save(const char *path, const NtfsJournal *state, char **error)
{
    if (!safe_journal_value(state->device) ||
        !safe_journal_value(state->target_identity) ||
        !safe_journal_value(state->stage) ||
        !safe_journal_value(state->plan)) {
        ntfs_set_error(error,
                       "NTFS transaction paths contain unsupported journal characters");
        return -1;
    }

    char *parent = ld_path_parent_directory(path);
    if (ensure_directory_tree(parent, error) != 0) {
        free(parent);
        return -1;
    }
    free(parent);

    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE, journal_write_stream, state);
    if (failure != 0) {
        ntfs_set_error(error, "cannot publish NTFS journal: %s",
                       strerror(failure));
        return -1;
    }
    return 0;
}

static char *value_copy(const char *value)
{
    const size_t length = strlen(value);
    return ld_xstrndup(value, length);
}

static int parse_u64(const char *text, uint64_t *value)
{
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

int ntfs_journal_load(const char *path, NtfsJournal *state, char **error)
{
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        ntfs_set_error(error, "cannot open NTFS recovery journal: %s",
                       strerror(errno));
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0U;
    if (getline(&line, &capacity, file) < 0)
        goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, NTFS_JOURNAL_MAGIC) != 0)
        goto invalid;

    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *value = NULL;
        if (infiltratr_config_parse_line(line, &key, &value) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto invalid;

        if (strcmp(key, "device") == 0) {
            free(state->device);
            state->device = value_copy(value);
        } else if (strcmp(key, "target_identity") == 0) {
            free(state->target_identity);
            state->target_identity = value_copy(value);
        } else if (strcmp(key, "serial") == 0) {
            infiltratr_copy_string(state->serial, sizeof(state->serial), value);
        } else if (strcmp(key, "operation") == 0) {
            infiltratr_copy_string(state->operation, sizeof(state->operation), value);
        } else if (strcmp(key, "phase") == 0) {
            infiltratr_copy_string(state->phase, sizeof(state->phase), value);
        } else if (strcmp(key, "stage") == 0) {
            free(state->stage);
            state->stage = value_copy(value);
        } else if (strcmp(key, "plan") == 0) {
            free(state->plan);
            state->plan = value_copy(value);
        } else if (strcmp(key, "physical_bytes") == 0 &&
                   parse_u64(value, &state->physical_bytes) != 0) {
            goto invalid;
        } else if (strcmp(key, "filesystem_bytes") == 0 &&
                   parse_u64(value, &state->filesystem_bytes) != 0) {
            goto invalid;
        } else if (strcmp(key, "commit_cluster") == 0 &&
                   parse_u64(value, &state->commit_cluster) != 0) {
            goto invalid;
        } else if (strcmp(key, "move_clusters") == 0 &&
                   parse_u64(value, &state->move_clusters) != 0) {
            goto invalid;
        } else if (strcmp(key, "workspace_start") == 0 &&
                   parse_u64(value, &state->workspace_start) != 0) {
            goto invalid;
        } else if (strcmp(key, "workspace_clusters") == 0 &&
                   parse_u64(value, &state->workspace_clusters) != 0) {
            goto invalid;
        }
    }

    free(line);
    (void)fclose(file);
    if (state->device == NULL || state->target_identity == NULL ||
        state->stage == NULL || state->plan == NULL ||
        state->serial[0] == '\0' || state->operation[0] == '\0' ||
        state->phase[0] == '\0' || state->physical_bytes == 0U ||
        state->filesystem_bytes == 0U)
        goto invalid_state;
    return 0;

invalid:
    free(line);
    (void)fclose(file);
invalid_state:
    ntfs_journal_free(state);
    ntfs_set_error(error, "NTFS recovery journal is malformed or incomplete");
    return -1;
}

int ntfs_journal_phase(const char *path, NtfsJournal *state,
                       const char *phase, char **error)
{
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return ntfs_journal_save(path, state, error);
}
