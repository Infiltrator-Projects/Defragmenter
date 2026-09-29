// SPDX-License-Identifier: GPL-3.0-or-later
/* Durable FAT journal serialization and parsing. */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ld_runtime.h"
#include "ld_path.h"
#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/posix.h"
#include "infiltratr/token.h"
#include "fat_journal.h"

void journal_free(Journal *j) {
    free(j->device_path);
    u32vec_free(&j->source);
    memset(j, 0, sizeof(*j));
}

void relocation_journal_free(RelocationJournal *j) {
    free(j->device_path);
    free(j->target_identity);
    free(j->moves);
    free(j->dir_patches);
    memset(j, 0, sizeof(*j));
}

void relocation_journal_add_move(RelocationJournal *j, RelocationMove move) {
    if (!infiltratr_array_reserve((void **)&j->moves, &j->move_capacity,
                                  sizeof(*j->moves), j->move_count + 1U, 64U))
        ld_die("cannot grow FAT relocation move list");
    j->moves[j->move_count++] = move;
}

void relocation_journal_add_dir_patch(RelocationJournal *j, RelocationDirPatch patch) {
    if (!infiltratr_array_reserve((void **)&j->dir_patches,
                                  &j->dir_patch_capacity,
                                  sizeof(*j->dir_patches),
                                  j->dir_patch_count + 1U, 64U))
        ld_die("cannot grow FAT relocation directory-patch list");
    j->dir_patches[j->dir_patch_count++] = patch;
}


static uint64_t parse_u64_value(const char *text, unsigned int base, const char *field) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64(text, base, &value)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "invalid FAT journal %s", field);
        ld_die(message);
    }
    return value;
}

static uint32_t parse_u32_value(const char *text, unsigned int base, const char *field) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(text, base, 0U, UINT32_MAX, &value)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "invalid FAT journal %s", field);
        ld_die(message);
    }
    return (uint32_t)value;
}

static size_t parse_size_value(const char *text, const char *field) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(text, 10U, 0U, (uint64_t)SIZE_MAX, &value)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "invalid FAT journal %s", field);
        ld_die(message);
    }
    return (size_t)value;
}

static JournalStage parse_stage_value(const char *text) {
    uint64_t value = 0;
    if (!infiltratr_parse_u64_range(text, 10U, J_PREPARED, J_OLD_FREED, &value))
        ld_die("invalid FAT journal stage");
    return (JournalStage)value;
}

static bool parse_csv_u64(const char *text, uint64_t *values, size_t count) {
    const char *cursor = text;
    for (size_t index = 0U; index < count; ++index) {
        if (!infiltratr_parse_u64_token(&cursor, 10U, &values[index]))
            return false;
        if (index + 1U < count) {
            if (*cursor != ',') return false;
            cursor++;
        }
    }
    return *cursor == '\0';
}

static bool relocation_journal_write_stream(FILE *file, const void *user_data) {
    const RelocationJournal *j = user_data;
    fprintf(file, "%s\n", RELOCATION_JOURNAL_MAGIC);
    fprintf(file, "device=%s\n", j->device_path);
    fprintf(file, "target_identity=%s\n", j->target_identity);
    fprintf(file, "physical_bytes=%" PRIu64 "\n", j->physical_bytes);
    fprintf(file, "volume_id=%08" PRIx32 "\n", j->volume_id);
    fprintf(file, "stage=%d\n", (int)j->stage);
    fprintf(file, "root_old=%" PRIu32 "\n", j->root_old);
    fprintf(file, "root_new=%" PRIu32 "\n", j->root_new);
    fprintf(file, "move_count=%zu\n", j->move_count);
    for (size_t i = 0; i < j->move_count; i++) {
        const RelocationMove *m = &j->moves[i];
        fprintf(file, "move=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                m->source, m->destination, m->next, m->predecessor);
    }
    fprintf(file, "dir_patch_count=%zu\n", j->dir_patch_count);
    for (size_t i = 0; i < j->dir_patch_count; i++) {
        const RelocationDirPatch *p = &j->dir_patches[i];
        fprintf(file, "dir_patch=%" PRIu64 ",%" PRIu32 ",%" PRIu32 "\n",
                p->offset, p->old_target, p->new_target);
    }
    return !ferror(file);
}

void relocation_journal_write(const char *path, const RelocationJournal *j) {
    char *parent = ld_path_parent_directory(path);
    if (ld_path_ensure_trusted_directory_tree(parent) != 0) {
        free(parent);
        ld_die_errno("create relocation journal directory");
    }
    free(parent);
    const int failure = infiltratr_atomic_file_write(
        path, INFILTRATR_ATOMIC_FILE_PRIVATE,
        relocation_journal_write_stream, j);
    if (failure != 0) {
        errno = failure;
        ld_die_errno("write relocation journal");
    }
}

bool journal_has_magic(const char *path, const char *magic) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) ld_die_errno("open journal");
    char *line = NULL;
    size_t cap = 0;
    bool match = getline(&line, &cap, fp) >= 0;
    if (match) infiltratr_trim_line_end(line);
    match = match && strcmp(line, magic) == 0;
    free(line);
    fclose(fp);
    return match;
}

Journal journal_read(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) ld_die_errno("open journal");
    Journal j = {0};
    char *line = NULL;
    size_t cap = 0;
    if (getline(&line, &cap, fp) < 0 || strcmp(line, JOURNAL_MAGIC "\n") != 0) {
        ld_die("invalid journal header");
    }
    size_t expected_count = 0;
    while (getline(&line, &cap, fp) >= 0) {
        char *key = NULL;
        char *eq = NULL;
        if (infiltratr_config_parse_line(line, &key, &eq) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            continue;
        if (strcmp(key, "device") == 0) j.device_path = ld_xstrdup(eq);
        else if (strcmp(key, "volume_id") == 0) j.volume_id = parse_u32_value(eq, 16U, "volume_id");
        else if (strcmp(key, "stage") == 0) j.stage = parse_stage_value(eq);
        else if (strcmp(key, "dirent_offset") == 0) j.dirent_offset = parse_u64_value(eq, 10U, "dirent_offset");
        else if (strcmp(key, "old_first") == 0) j.old_first = parse_u32_value(eq, 10U, "old_first");
        else if (strcmp(key, "dest_start") == 0) j.dest_start = parse_u32_value(eq, 10U, "dest_start");
        else if (strcmp(key, "count") == 0) expected_count = parse_size_value(eq, "count");
        else if (strcmp(key, "source") == 0) {
            const char *cursor = eq;
            if (*cursor == '\0') {
                journal_free(&j);
                ld_die("invalid FAT journal source cluster list");
            }
            for (;;) {
                uint64_t value = 0U;
                if (!infiltratr_parse_u64_token(&cursor, 10U, &value) ||
                    value > UINT32_MAX) {
                    journal_free(&j);
                    ld_die("invalid FAT journal source cluster list");
                }
                u32vec_push(&j.source, (uint32_t)value);
                if (*cursor == '\0') break;
                if (*cursor != ',') {
                    journal_free(&j);
                    ld_die("invalid FAT journal source cluster list");
                }
                cursor++;
                if (*cursor == '\0') {
                    journal_free(&j);
                    ld_die("invalid FAT journal source cluster list");
                }
            }
        }
    }
    free(line);
    fclose(fp);
    if (j.device_path == NULL || j.source.len != expected_count || j.source.len == 0) {
        journal_free(&j);
        ld_die("journal is incomplete or corrupt");
    }
    return j;
}

RelocationJournal relocation_journal_read(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) ld_die_errno("open relocation journal");
    RelocationJournal j = {0};
    char *line = NULL;
    size_t cap = 0;
    if (getline(&line, &cap, fp) < 0 || strcmp(line, RELOCATION_JOURNAL_MAGIC "\n") != 0) {
        ld_die("invalid relocation journal header");
    }
    size_t expected_moves = 0;
    size_t expected_patches = 0;
    while (getline(&line, &cap, fp) >= 0) {
        char *key = NULL;
        char *eq = NULL;
        if (infiltratr_config_parse_line(line, &key, &eq) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            continue;
        if (strcmp(key, "device") == 0) j.device_path = ld_xstrdup(eq);
        else if (strcmp(key, "target_identity") == 0) j.target_identity = ld_xstrdup(eq);
        else if (strcmp(key, "physical_bytes") == 0) j.physical_bytes = parse_u64_value(eq, 10U, "physical_bytes");
        else if (strcmp(key, "volume_id") == 0) j.volume_id = parse_u32_value(eq, 16U, "volume_id");
        else if (strcmp(key, "stage") == 0) j.stage = parse_stage_value(eq);
        else if (strcmp(key, "root_old") == 0) j.root_old = parse_u32_value(eq, 10U, "root_old");
        else if (strcmp(key, "root_new") == 0) j.root_new = parse_u32_value(eq, 10U, "root_new");
        else if (strcmp(key, "move_count") == 0) expected_moves = parse_size_value(eq, "move_count");
        else if (strcmp(key, "dir_patch_count") == 0) {
            expected_patches = parse_size_value(eq, "dir_patch_count");
        } else if (strcmp(key, "move") == 0) {
            uint64_t fields[4] = {0};
            if (!parse_csv_u64(eq, fields, 4U) ||
                fields[0] > UINT32_MAX || fields[1] > UINT32_MAX ||
                fields[2] > UINT32_MAX || fields[3] > UINT32_MAX) {
                relocation_journal_free(&j);
                ld_die("invalid relocation journal move record");
            }
            RelocationMove m = {
                .source = (uint32_t)fields[0],
                .destination = (uint32_t)fields[1],
                .next = (uint32_t)fields[2],
                .predecessor = (uint32_t)fields[3],
            };
            relocation_journal_add_move(&j, m);
        } else if (strcmp(key, "dir_patch") == 0) {
            uint64_t fields[3] = {0};
            if (!parse_csv_u64(eq, fields, 3U) ||
                fields[1] > UINT32_MAX || fields[2] > UINT32_MAX) {
                relocation_journal_free(&j);
                ld_die("invalid relocation journal directory patch");
            }
            RelocationDirPatch p = {
                .offset = fields[0],
                .old_target = (uint32_t)fields[1],
                .new_target = (uint32_t)fields[2],
            };
            relocation_journal_add_dir_patch(&j, p);
        }
    }
    free(line);
    fclose(fp);
    if (j.device_path == NULL || j.target_identity == NULL ||
        j.physical_bytes == 0U || j.move_count == 0 ||
        j.move_count != expected_moves || j.dir_patch_count != expected_patches ||
        j.root_old < 2 || j.root_new < 2) {
        relocation_journal_free(&j);
        ld_die("relocation journal is incomplete or corrupt");
    }
    return j;
}

void journal_remove(const char *path) {
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0) {
        errno = failure;
        ld_die_errno("durably remove journal");
    }
}

bool path_exists(const char *path) {
    return access(path, F_OK) == 0;
}
