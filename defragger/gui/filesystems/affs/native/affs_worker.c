// SPDX-License-Identifier: GPL-3.0-or-later
#include "affs_native.h"
#include "version.h"

#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/posix.h"
#include "ld_device.h"
#include "ld_io.h"
#include "ld_path.h"
#include "ld_runtime.h"
#include "ld_protocol.h"
#include "ld_stop.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PROG "linux-defragger-affs-worker"
#define JOURNAL_MAGIC "LINUX-DEFRAGGER-AFFS-JOURNAL-2"
#define BLOCK_SIZE 512U
#define AMIGA_IO_BATCH_BLOCKS 8192U
#define AMIGA_IO_BATCH_BYTES ((size_t)AMIGA_IO_BATCH_BLOCKS * BLOCK_SIZE)
#define STOPPED 130

typedef struct {
    char *device;
    char *target_identity;
    char *stage;
    char operation[24];
    char phase[24];
    char volume_token[65];
    char stage_sha256[65];
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint32_t blocks;
    uint32_t root_block;
    uint8_t dostype;
} AffsJournal;

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
        "defrag|growth-defrag|recover DEVICE --write --confirm DEVICE --journal PATH "
        "[--growth-percent 10] [--live-updates]\n", PROG);
}



static bool parse_unsigned(const char *text, unsigned *value) {
    uint64_t parsed = 0;
    if (!infiltratr_parse_u64_range(text, 10U, 0U, UINT_MAX, &parsed)) return false;
    *value = (unsigned)parsed;
    return true;
}

static bool safe_value(const char *value) {
    return value != NULL && strchr(value, '\n') == NULL &&
           strchr(value, '\r') == NULL && strchr(value, '=') == NULL;
}

static void unlink_if_exists(const char *path) {
    if (path == NULL || *path == '\0') return;
    const int failure = infiltratr_unlink_durable(path, true);
    if (failure != 0)
        fprintf(stderr, "%s: warning: cannot durably remove %s: %s\n",
                PROG, path, strerror(failure));
}


static void journal_free(AffsJournal *state) {
    if (state == NULL) return;
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static int ensure_directory_tree(const char *path, char **error) {
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        affs_set_error(error, "cannot create Amiga journal directory %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static bool journal_write_stream(FILE *file, const void *user_data) {
    const AffsJournal *state = user_data;
    fprintf(file, "%s\n", JOURNAL_MAGIC);
    fprintf(file, "device=%s\n", state->device);
    fprintf(file, "target_identity=%s\n", state->target_identity);
    fprintf(file, "stage=%s\n", state->stage);
    fprintf(file, "operation=%s\n", state->operation);
    fprintf(file, "phase=%s\n", state->phase);
    fprintf(file, "volume_token=%s\n", state->volume_token);
    fprintf(file, "stage_sha256=%s\n", state->stage_sha256);
    fprintf(file, "physical_bytes=%" PRIu64 "\n", state->physical_bytes);
    fprintf(file, "filesystem_bytes=%" PRIu64 "\n", state->filesystem_bytes);
    fprintf(file, "blocks=%u\n", state->blocks);
    fprintf(file, "root_block=%u\n", state->root_block);
    fprintf(file, "dostype=%u\n", (unsigned)state->dostype);
    return !ferror(file);
}

static int journal_save(const char *path, const AffsJournal *state, char **error) {
    if (!safe_value(state->device) || !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        affs_set_error(error, "Amiga transaction paths contain unsupported journal characters");
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
        affs_set_error(error, "cannot publish Amiga recovery journal: %s",
                       strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64(const char *text, uint64_t *value) {
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, AffsJournal *state, char **error) {
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        affs_set_error(error, "cannot open Amiga recovery journal: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, JOURNAL_MAGIC) != 0) {
        if (strcmp(line, "LINUX-DEFRAGGER-AFFS-1") == 0) {
            free(line);
            fclose(file);
            affs_set_error(error,
                "legacy Amiga recovery journal lacks target identity and stage integrity data; refusing automatic recovery");
            return -1;
        }
        goto invalid;
    }
    while (getline(&line, &capacity, file) >= 0) {
        char *key = NULL;
        char *equals = NULL;
        if (infiltratr_config_parse_line(line, &key, &equals) !=
            INFILTRATR_CONFIG_LINE_ENTRY)
            goto invalid;
        if (strcmp(key, "device") == 0) {
            free(state->device); state->device = ld_xstrdup(equals);
        } else if (strcmp(key, "target_identity") == 0) {
            free(state->target_identity); state->target_identity = ld_xstrdup(equals);
        } else if (strcmp(key, "stage") == 0) {
            free(state->stage); state->stage = ld_xstrdup(equals);
        } else if (strcmp(key, "operation") == 0) {
            infiltratr_copy_string(state->operation, sizeof(state->operation), equals);
        } else if (strcmp(key, "phase") == 0) {
            infiltratr_copy_string(state->phase, sizeof(state->phase), equals);
        } else if (strcmp(key, "volume_token") == 0) {
            infiltratr_copy_string(state->volume_token, sizeof(state->volume_token), equals);
        } else if (strcmp(key, "stage_sha256") == 0) {
            infiltratr_copy_string(state->stage_sha256, sizeof(state->stage_sha256), equals);
        } else if (strcmp(key, "physical_bytes") == 0) {
            if (parse_u64(equals, &state->physical_bytes) != 0) goto invalid;
        } else if (strcmp(key, "filesystem_bytes") == 0) {
            if (parse_u64(equals, &state->filesystem_bytes) != 0) goto invalid;
        } else if (strcmp(key, "blocks") == 0) {
            uint64_t value = 0;
            if (parse_u64(equals, &value) != 0 || value > UINT32_MAX) goto invalid;
            state->blocks = (uint32_t)value;
        } else if (strcmp(key, "root_block") == 0) {
            uint64_t value = 0;
            if (parse_u64(equals, &value) != 0 || value > UINT32_MAX) goto invalid;
            state->root_block = (uint32_t)value;
        } else if (strcmp(key, "dostype") == 0) {
            uint64_t value = 0;
            if (parse_u64(equals, &value) != 0 || value > UINT8_MAX) goto invalid;
            state->dostype = (uint8_t)value;
        }
    }
    free(line);
    fclose(file);
    if (state->device == NULL || state->target_identity == NULL || state->stage == NULL ||
        state->operation[0] == '\0' || state->phase[0] == '\0' ||
        strlen(state->volume_token) != 64U || strlen(state->stage_sha256) != 64U ||
        state->physical_bytes == 0 || state->filesystem_bytes == 0 ||
        state->blocks == 0 || state->root_block == 0) goto invalid_state;
    return 0;
invalid:
    free(line);
    fclose(file);
invalid_state:
    journal_free(state);
    affs_set_error(error, "Amiga recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, AffsJournal *state,
                         const char *phase, char **error) {
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error);
}

static void transaction_cleanup(const char *journal, const AffsJournal *state) {
    if (state != NULL) unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static char *stage_name(const char *journal) {
    return ld_path_append_suffix(journal, ".affs-stage");
}

static int digest_final_hex(EVP_MD_CTX *context, char output[65], char **error) {
    unsigned char digest[32];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32U) {
        affs_set_error(error, "finalising Amiga SHA-256 failed");
        return -1;
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        output[i * 2U] = digits[digest[i] >> 4];
        output[i * 2U + 1U] = digits[digest[i] & 15U];
    }
    output[64] = '\0';
    return 0;
}

static int volume_token(const char *path, uint64_t physical_bytes,
                        uint32_t *blocks, uint32_t *root_block,
                        uint8_t *dostype, uint64_t *filesystem_bytes,
                        char output[65], char **error) {
    if ((physical_bytes % BLOCK_SIZE) != 0U ||
        physical_bytes / BLOCK_SIZE > UINT32_MAX ||
        physical_bytes / BLOCK_SIZE < 20U) {
        affs_set_error(error, "Amiga target identity check found unsupported geometry");
        return -1;
    }
    uint32_t found_blocks = (uint32_t)(physical_bytes / BLOCK_SIZE);
    uint32_t found_root = found_blocks / 2U;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        affs_set_error(error, "cannot open Amiga target for identity verification: %s", strerror(errno));
        return -1;
    }
    unsigned char boot0[BLOCK_SIZE];
    unsigned char boot1[BLOCK_SIZE];
    unsigned char root[BLOCK_SIZE];
    ssize_t a = ld_pread_full(fd, boot0, sizeof(boot0), 0U);
    ssize_t b = ld_pread_full(fd, boot1, sizeof(boot1), BLOCK_SIZE);
    ssize_t c = ld_pread_full(fd, root, sizeof(root), (uint64_t)found_root * BLOCK_SIZE);
    close(fd);
    if (a != (ssize_t)sizeof(boot0) || b != (ssize_t)sizeof(boot1) ||
        c != (ssize_t)sizeof(root) || memcmp(boot0, "DOS", 3U) != 0 || boot0[3] > 7U) {
        affs_set_error(error, "Amiga target identity check found a different or malformed filesystem");
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(context, boot0, sizeof(boot0)) != 1 ||
        EVP_DigestUpdate(context, boot1, sizeof(boot1)) != 1 ||
        EVP_DigestUpdate(context, root, sizeof(root)) != 1) {
        EVP_MD_CTX_free(context);
        affs_set_error(error, "initialising Amiga volume identity SHA-256 failed");
        return -1;
    }
    int rc = digest_final_hex(context, output, error);
    EVP_MD_CTX_free(context);
    if (rc != 0) return -1;
    *blocks = found_blocks;
    *root_block = found_root;
    *dostype = boot0[3];
    *filesystem_bytes = (uint64_t)found_blocks * BLOCK_SIZE;
    return 0;
}

static uint32_t allocated_run_blocks(const AffsVolume *stage, uint32_t start) {
    uint32_t count = 0U;
    while (start + count < stage->blocks && count < AMIGA_IO_BATCH_BLOCKS &&
           !ld_bitmap_get(stage->free_map, (uint64_t)start + count)) {
        ++count;
    }
    return count;
}

static int stage_sha256(const char *path, char output[65], char **error) {
    AffsVolume stage;
    if (affs_scan(path, false, &stage, error) != 0) return -1;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char *buffer = malloc(AMIGA_IO_BATCH_BYTES);
    if (context == NULL || buffer == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        free(buffer);
        EVP_MD_CTX_free(context);
        affs_close(&stage);
        affs_set_error(error, "initialising Amiga stage SHA-256 failed");
        return -1;
    }
    printf("Amiga stage integrity: hashing allocated runs in batches up to %u KiB.\n",
           (unsigned)(AMIGA_IO_BATCH_BYTES / 1024U));
    fflush(stdout);
    int rc = 0;
    for (uint32_t i = 0; i < stage.blocks;) {
        if (ld_bitmap_get(stage.free_map, i)) {
            ++i;
            continue;
        }
        if (ld_stop_requested()) {
            rc = STOPPED;
            break;
        }
        uint32_t count = allocated_run_blocks(&stage, i);
        size_t bytes = (size_t)count * BLOCK_SIZE;
        uint64_t offset = (uint64_t)i * BLOCK_SIZE;
        ssize_t got = ld_pread_full(stage.fd, buffer, bytes, offset);
        if (got != (ssize_t)bytes || EVP_DigestUpdate(context, buffer, bytes) != 1) {
            affs_set_error(error, "hashing verified Amiga stage blocks %u..%u failed", i, i + count);
            rc = -1;
            break;
        }
        i += count;
    }
    if (rc == 0) rc = digest_final_hex(context, output, error);
    free(buffer);
    EVP_MD_CTX_free(context);
    affs_close(&stage);
    return rc;
}

static int capture_target(const char *device, AffsJournal *state, char **error) {
    if (ld_device_capture_binding(device, &state->device,
                                  &state->target_identity,
                                  &state->physical_bytes) != 0) {
        affs_set_error(error, "cannot bind Amiga target: %s", strerror(errno));
        return -1;
    }
    return volume_token(state->device, state->physical_bytes,
                        &state->blocks, &state->root_block, &state->dostype,
                        &state->filesystem_bytes, state->volume_token, error);
}

static int check_unchanged_target(const char *device,
                                  const AffsJournal *state, char **error) {
    char *canonical = NULL;
    char *identity = NULL;
    uint64_t physical_bytes = 0;
    int rc = ld_device_capture_binding(
        device, &canonical, &identity, &physical_bytes);
    if (rc != 0) {
        affs_set_error(error, "cannot rebind Amiga target: %s", strerror(errno));
        return -1;
    }
    if (strcmp(canonical, state->device) != 0 ||
        strcmp(identity, state->target_identity) != 0 ||
        physical_bytes != state->physical_bytes) {
        affs_set_error(error, "Amiga target path, identity or capacity changed before source commit");
        free(identity);
        free(canonical);
        return -1;
    }
    uint32_t blocks = 0, root_block = 0;
    uint8_t dostype = 0;
    uint64_t filesystem_bytes = 0;
    char token[65];
    rc = volume_token(canonical, physical_bytes, &blocks, &root_block,
                      &dostype, &filesystem_bytes, token, error);
    free(identity);
    free(canonical);
    if (rc != 0) return -1;
    if (blocks != state->blocks || root_block != state->root_block ||
        dostype != state->dostype || filesystem_bytes != state->filesystem_bytes ||
        strcmp(token, state->volume_token) != 0) {
        affs_set_error(error, "Amiga filesystem identity changed before source commit");
        return -1;
    }
    return 0;
}

static int stop_commit(int target, char **error) {
    if (ld_sync_fd(target) != 0) {
        affs_set_error(error, "cannot sync Amiga source at Stop boundary: %s", strerror(errno));
        return -1;
    }
    return STOPPED;
}

static int safe_commit_stage(const char *stage_path, const char *target_path,
                             const AffsJournal *state,
                             uint64_t *written, char **error) {
    AffsVolume stage;
    if (affs_scan(stage_path, false, &stage, error) != 0) return -1;
    int target = ld_device_open_verified_fd(target_path, true,
                                            state->target_identity,
                                            state->physical_bytes);
    if (target < 0) {
        affs_set_error(error, "cannot open Amiga source for commit: %s", strerror(errno));
        affs_close(&stage);
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        affs_set_error(error, "cannot lock Amiga source for commit: %s", strerror(errno));
        close(target);
        affs_close(&stage);
        return -1;
    }
    unsigned char *buffer = malloc(AMIGA_IO_BATCH_BYTES);
    if (buffer == NULL) {
        affs_set_error(error, "out of memory batching Amiga source commit");
        (void)flock(target, LOCK_UN);
        close(target);
        affs_close(&stage);
        return -1;
    }
    printf("Amiga source commit batching: up to %u KiB per contiguous I/O batch.\n",
           (unsigned)(AMIGA_IO_BATCH_BYTES / 1024U));
    fflush(stdout);
    uint64_t total_written = 0;
    int rc = 0;
    for (uint32_t i = 0; i < stage.blocks;) {
        if (ld_bitmap_get(stage.free_map, i)) {
            ++i;
            continue;
        }
        if (ld_stop_requested()) {
            rc = stop_commit(target, error);
            break;
        }
        uint32_t count = allocated_run_blocks(&stage, i);
        size_t bytes = (size_t)count * BLOCK_SIZE;
        uint64_t offset = (uint64_t)i * BLOCK_SIZE;
        ssize_t got = ld_pread_full(stage.fd, buffer, bytes, offset);
        ssize_t put = got == (ssize_t)bytes
            ? ld_pwrite_full(target, buffer, bytes, offset) : -1;
        if (got != (ssize_t)bytes || put != (ssize_t)bytes) {
            affs_set_error(error, "short I/O committing Amiga blocks %u..%u", i, i + count);
            rc = -1;
            break;
        }
        total_written += bytes;
        i += count;
    }
    if (rc == 0 && ld_stop_requested()) rc = stop_commit(target, error);
    if (rc == 0 && ld_sync_fd(target) != 0) {
        affs_set_error(error, "cannot sync Amiga source: %s", strerror(errno));
        rc = -1;
    }
    free(buffer);
    (void)flock(target, LOCK_UN);
    close(target);
    affs_close(&stage);
    if (written != NULL) *written = total_written;
    return rc;
}

static bool valid_operation(const char *operation) {
    return strcmp(operation, "defrag") == 0 || strcmp(operation, "growth-defrag") == 0;
}

static bool valid_phase(const char *phase) {
    return strcmp(phase, "staged") == 0 || strcmp(phase, "committing") == 0 ||
           strcmp(phase, "committed") == 0;
}

static int handle_recovery(const char *device, const char *journal,
                           bool live, char **error) {
    AffsJournal state;
    if (journal_load(journal, &state, error) != 0) return 1;
    if (!ld_path_is_derived_from(state.stage, journal, ".affs-stage")) {
        affs_set_error(error,
            "Amiga recovery stage is not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    if (!valid_operation(state.operation) || !valid_phase(state.phase)) {
        affs_set_error(error, "Amiga recovery journal contains an unsupported operation or phase");
        journal_free(&state);
        return 1;
    }
    if (check_unchanged_target(device, &state, error) != 0) {
        journal_free(&state);
        return 1;
    }
    char digest[65];
    int digest_rc = stage_sha256(state.stage, digest, error);
    if (digest_rc == STOPPED) {
        ld_emit_result_event(stdout, "recover", "stopped", "Stopped before source writes; recovery artifacts remain intact.");
        journal_free(&state);
        return STOPPED;
    }
    if (digest_rc != 0 || strcmp(digest, state.stage_sha256) != 0) {
        if (digest_rc == 0)
            affs_set_error(error, "Amiga recovery stage SHA-256 does not match the persisted transaction");
        journal_free(&state);
        return 1;
    }
    bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (affs_verify_layout(state.stage, growth, 10U, error) != 0) {
        journal_free(&state);
        return 1;
    }
    if (strcmp(state.phase, "committed") == 0) {
        if (affs_verify_layout(device, growth, 10U, error) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, "recover", "completed", "Verified an already committed Amiga transaction.");
        journal_free(&state);
        return 0;
    }
    if (journal_phase(journal, &state, "committing", error) != 0) {
        journal_free(&state);
        return 1;
    }
    uint64_t written = 0;
    int commit_rc = safe_commit_stage(state.stage, device, &state, &written, error);
    if (commit_rc == STOPPED) {
        printf("Amiga recovery stopped at a durable source-write boundary; journal and verified stage were retained.\n");
        ld_emit_result_event(stdout, "recover", "stopped", "Recovery can be resumed safely.");
        journal_free(&state);
        return STOPPED;
    }
    if (commit_rc != 0 || affs_verify_layout(device, growth, 10U, error) != 0) {
        journal_free(&state);
        return 1;
    }
    if (journal_phase(journal, &state, "committed", error) != 0) {
        journal_free(&state);
        return 1;
    }
    transaction_cleanup(journal, &state);
    if (live) {
        printf("@@LIVE_RESET {\"reason\":\"authoritative post-recovery Amiga map\"}\n");
        fflush(stdout);
    }
    printf("Recovered verified Amiga source; committed %" PRIu64 " KiB of allocated blocks.\n",
           written / 1024U);
    ld_emit_result_event(stdout, "recover", "completed", "");
    journal_free(&state);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts(LD_VERSION);
        return 0;
    }
    if (argc < 3) {
        usage(stderr);
        return 2;
    }
    const char *mode = argv[1];
    const char *device = argv[2];
    char *error = NULL;
    if (strcmp(mode, "identify") == 0) {
        AffsVolume volume;
        if (affs_scan(device, false, &volume, &error) != 0) {
            free(error);
            return 1;
        }
        printf("{\"filesystem\":\"affs\",\"variant\":\"%s\",\"dostype\":%u}\n",
               volume.ffs ? "FFS" : "OFS", (unsigned)volume.dostype);
        affs_close(&volume);
        return 0;
    }
    if (strcmp(mode, "analyse-json") == 0) {
        int rc = affs_analyse_json(device, &error);
        if (rc != 0) {
            fprintf(stderr, "%s\n", error == NULL ? "Amiga analysis failed" : error);
            free(error);
            return 1;
        }
        return 0;
    }

    bool growth = strcmp(mode, "growth-defrag") == 0;
    bool defrag = strcmp(mode, "defrag") == 0;
    bool recover = strcmp(mode, "recover") == 0;
    if (!growth && !defrag && !recover) {
        usage(stderr);
        return 2;
    }
    const char *confirm = NULL;
    const char *journal = NULL;
    unsigned growth_percent = 10U;
    bool write = false;
    bool live = false;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--write") == 0) write = true;
        else if (strcmp(argv[i], "--live-updates") == 0) live = true;
        else if (strcmp(argv[i], "--confirm") == 0 && i + 1 < argc) confirm = argv[++i];
        else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) journal = argv[++i];
        else if (strcmp(argv[i], "--growth-percent") == 0 && i + 1 < argc) {
            if (!parse_unsigned(argv[++i], &growth_percent)) {
                fprintf(stderr, "invalid --growth-percent\n");
                return 2;
            }
        } else if ((strcmp(argv[i], "--workers") == 0 ||
                    strcmp(argv[i], "--ram-buffer") == 0 ||
                    strcmp(argv[i], "--batch-clusters") == 0 ||
                    strcmp(argv[i], "--live-map-cells") == 0) && i + 1 < argc) {
            ++i;
        } else {
            fprintf(stderr, "unknown or incomplete Amiga option: %s\n", argv[i]);
            return 2;
        }
    }
    if (!write || journal == NULL || confirm == NULL || strcmp(confirm, device) != 0) {
        fprintf(stderr, "Amiga mutation requires --write --confirm DEVICE --journal PATH\n");
        return 2;
    }
    if (growth && growth_percent != 10U) {
        fprintf(stderr, "Amiga Growth Defrag requires exactly 10 percent reserve\n");
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        fprintf(stderr,
            "Amiga target is mounted; raw mutation and recovery require an unmounted filesystem\n");
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    if (recover) {
        int rc = handle_recovery(device, journal, live, &error);
        if (rc != 0 && rc != STOPPED)
            fprintf(stderr, "%s\n", error == NULL ? "Amiga recovery failed" : error);
        free(error);
        return rc;
    }

    AffsJournal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation, sizeof(state.operation), mode);
    if (capture_target(device, &state, &error) != 0) goto fail;
    state.stage = stage_name(journal);
    if (state.stage == NULL) {
        affs_set_error(&error, "out of memory creating Amiga stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 || access(state.stage, F_OK) == 0) {
        affs_set_error(&error,
            "existing Amiga recovery artifacts must be recovered or removed before starting a new transaction");
        goto fail;
    }

    printf("Starting native C Amiga %s on %s.\n",
           growth ? "Growth Defrag" : "Defrag", device);
    uint64_t planned = 0;
    if (affs_build_stage(device, state.stage, growth, growth_percent,
                         live, &planned, &error) != 0 ||
        affs_verify_layout(state.stage, growth, growth_percent, &error) != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    if (ld_stop_requested()) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(stdout, mode, "stopped", "Stopped before any source writes.");
        journal_free(&state);
        return STOPPED;
    }
    int hash_rc = stage_sha256(state.stage, state.stage_sha256, &error);
    if (hash_rc == STOPPED) {
        unlink_if_exists(state.stage);
        ld_emit_result_event(stdout, mode, "stopped", "Stopped before any source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (hash_rc != 0 || check_unchanged_target(device, &state, &error) != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    infiltratr_copy_string(state.phase, sizeof(state.phase), "staged");
    if (journal_save(journal, &state, &error) != 0) {
        unlink_if_exists(state.stage);
        goto fail;
    }
    if (ld_stop_requested()) {
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, mode, "stopped", "Stopped before any source writes.");
        journal_free(&state);
        return STOPPED;
    }
    if (journal_phase(journal, &state, "committing", &error) != 0) goto fail;
    printf("Amiga source commit: writing %" PRIu64 " KiB of verified allocated blocks only.\n",
           planned / 1024U);
    fflush(stdout);
    uint64_t written = 0;
    int commit_rc = safe_commit_stage(state.stage, device, &state, &written, &error);
    if (commit_rc == STOPPED) {
        printf("Amiga Stop reached a durable source-write boundary; recovery journal and verified stage were retained.\n");
        ld_emit_result_event(stdout, mode, "stopped", "Run Recover to resume the verified transaction.");
        journal_free(&state);
        free(error);
        return STOPPED;
    }
    if (commit_rc != 0 || affs_verify_layout(device, growth, growth_percent, &error) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committed", &error) != 0) goto fail;
    transaction_cleanup(journal, &state);
    if (live) {
        printf("@@LIVE_RESET {\"reason\":\"authoritative post-commit Amiga map\"}\n");
        fflush(stdout);
    }
    printf("Amiga %s completed; committed %" PRIu64 " KiB of allocated blocks.\n",
           growth ? "Growth Defrag" : "Defrag", written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    free(error);
    return 0;

fail:
    fprintf(stderr, "%s\n", error == NULL ? "Amiga transaction failed" : error);
    free(error);
    journal_free(&state);
    return 1;
}
