// SPDX-License-Identifier: GPL-3.0-or-later
#include "hfsplus_native.h"
#include "version.h"

#include "infiltratr/core.h"
#include "infiltratr/config.h"
#include "infiltratr/posix.h"
#include "infiltratr/endian.h"
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

#define PROG "linux-defragger-hfsplus-worker"
#define JOURNAL_MAGIC "LINUX-DEFRAGGER-HFSPLUS-JOURNAL-2"
#define HASH_CHUNK (1024U * 1024U)
#define COMMIT_CHUNK (4U * 1024U * 1024U)
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
    uint32_t block_size;
    uint32_t total_blocks;
    uint16_t signature;
    uint16_t version;
} HfsPlusJournal;

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


static void journal_free(HfsPlusJournal *state) {
    if (state == NULL) return;
    free(state->device);
    free(state->target_identity);
    free(state->stage);
    memset(state, 0, sizeof(*state));
}

static int ensure_directory_tree(const char *path, char **error) {
    if (ld_path_ensure_trusted_directory_tree(path) != 0) {
        hfsplus_set_error(error, "cannot create HFS+ journal directory %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static bool journal_write_stream(FILE *file, const void *user_data) {
    const HfsPlusJournal *state = user_data;
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
    fprintf(file, "block_size=%u\n", state->block_size);
    fprintf(file, "total_blocks=%u\n", state->total_blocks);
    fprintf(file, "signature=%u\n", (unsigned)state->signature);
    fprintf(file, "version=%u\n", (unsigned)state->version);
    return !ferror(file);
}

static int journal_save(const char *path, const HfsPlusJournal *state, char **error) {
    if (!safe_value(state->device) || !safe_value(state->target_identity) ||
        !safe_value(state->stage)) {
        hfsplus_set_error(error, "HFS+ transaction paths contain unsupported journal characters");
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
        hfsplus_set_error(error, "cannot publish HFS+ recovery journal: %s",
                          strerror(failure));
        return -1;
    }
    return 0;
}

static int parse_u64(const char *text, uint64_t *value) {
    return infiltratr_parse_u64(text, 10U, value) ? 0 : -1;
}

static int journal_load(const char *path, HfsPlusJournal *state, char **error) {
    memset(state, 0, sizeof(*state));
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        hfsplus_set_error(error, "cannot open HFS+ recovery journal: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0) goto invalid;
    infiltratr_trim_line_end(line);
    if (strcmp(line, JOURNAL_MAGIC) != 0) {
        if (strcmp(line, "LINUX-DEFRAGGER-HFSPLUS-1") == 0) {
            free(line);
            fclose(file);
            hfsplus_set_error(error,
                "legacy HFS+ recovery journal lacks target identity and stage integrity data; refusing automatic recovery");
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
        } else if (strcmp(key, "block_size") == 0) {
            uint64_t value = 0;
            if (parse_u64(equals, &value) != 0 || value > UINT32_MAX) goto invalid;
            state->block_size = (uint32_t)value;
        } else if (strcmp(key, "total_blocks") == 0) {
            uint64_t value = 0;
            if (parse_u64(equals, &value) != 0 || value > UINT32_MAX) goto invalid;
            state->total_blocks = (uint32_t)value;
        } else if (strcmp(key, "signature") == 0) {
            uint64_t value = 0;
            if (parse_u64(equals, &value) != 0 || value > UINT16_MAX) goto invalid;
            state->signature = (uint16_t)value;
        } else if (strcmp(key, "version") == 0) {
            uint64_t value = 0;
            if (parse_u64(equals, &value) != 0 || value > UINT16_MAX) goto invalid;
            state->version = (uint16_t)value;
        }
    }
    free(line);
    fclose(file);
    if (state->device == NULL || state->target_identity == NULL || state->stage == NULL ||
        state->operation[0] == '\0' || state->phase[0] == '\0' ||
        strlen(state->volume_token) != 64U || strlen(state->stage_sha256) != 64U ||
        state->physical_bytes == 0 || state->filesystem_bytes == 0 ||
        state->block_size == 0 || state->total_blocks == 0 ||
        state->signature == 0 || state->version == 0) goto invalid_state;
    return 0;
invalid:
    free(line);
    fclose(file);
invalid_state:
    journal_free(state);
    hfsplus_set_error(error, "HFS+ recovery journal is malformed or incomplete");
    return -1;
}

static int journal_phase(const char *path, HfsPlusJournal *state,
                         const char *phase, char **error) {
    infiltratr_copy_string(state->phase, sizeof(state->phase), phase);
    return journal_save(path, state, error);
}

static void transaction_cleanup(const char *journal, const HfsPlusJournal *state) {
    if (state != NULL) unlink_if_exists(state->stage);
    unlink_if_exists(journal);
}

static char *stage_name(const char *journal) {
    return ld_path_append_suffix(journal, ".hfsplus-stage");
}

static int digest_final_hex(EVP_MD_CTX *context, char output[65], char **error) {
    unsigned char digest[32];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32U) {
        hfsplus_set_error(error, "finalising HFS+ SHA-256 failed");
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
                        uint16_t *signature, uint16_t *version,
                        uint32_t *block_size, uint32_t *total_blocks,
                        uint64_t *filesystem_bytes, char output[65], char **error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hfsplus_set_error(error, "cannot open HFS+ target for identity verification: %s", strerror(errno));
        return -1;
    }
    unsigned char header[512];
    ssize_t got = ld_pread_full(fd, header, sizeof(header), 1024U);
    close(fd);
    if (got != (ssize_t)sizeof(header)) {
        hfsplus_set_error(error, "cannot read HFS+ volume header for identity verification");
        return -1;
    }
    uint16_t found_signature = infiltratr_load_be16(header);
    uint16_t found_version = infiltratr_load_be16(header + 2U);
    if (!((found_signature == 0x482bU && found_version == 4U) ||
          (found_signature == 0x4858U && found_version == 5U))) {
        hfsplus_set_error(error, "HFS+ target identity check found a different filesystem");
        return -1;
    }
    uint32_t found_block_size = infiltratr_load_be32(header + 40U);
    uint32_t found_total_blocks = infiltratr_load_be32(header + 44U);
    if (found_block_size < 512U ||
        (found_block_size & (found_block_size - 1U)) != 0U ||
        found_total_blocks == 0U) {
        hfsplus_set_error(error, "HFS+ target identity check found invalid allocation geometry");
        return -1;
    }
    uint64_t found_filesystem_bytes = (uint64_t)found_block_size * found_total_blocks;
    if (found_filesystem_bytes > physical_bytes) {
        hfsplus_set_error(error, "HFS+ filesystem geometry exceeds the target during identity verification");
        return -1;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(context, header, sizeof(header)) != 1) {
        EVP_MD_CTX_free(context);
        hfsplus_set_error(error, "initialising HFS+ volume identity SHA-256 failed");
        return -1;
    }
    int rc = digest_final_hex(context, output, error);
    EVP_MD_CTX_free(context);
    if (rc != 0) return -1;
    *signature = found_signature;
    *version = found_version;
    *block_size = found_block_size;
    *total_blocks = found_total_blocks;
    *filesystem_bytes = found_filesystem_bytes;
    return 0;
}

static int hash_update_region(EVP_MD_CTX *context, int fd, uint64_t offset,
                              uint64_t length, unsigned char *buffer,
                              size_t buffer_size, char **error) {
    uint64_t done = 0;
    while (done < length) {
        if (ld_stop_requested()) return STOPPED;
        size_t take = buffer_size;
        if ((uint64_t)take > length - done) take = (size_t)(length - done);
        ssize_t got = ld_pread_full(fd, buffer, take, offset + done);
        if (got != (ssize_t)take || EVP_DigestUpdate(context, buffer, take) != 1) {
            hfsplus_set_error(error, "hashing verified HFS+ stage failed");
            return -1;
        }
        done += take;
    }
    return 0;
}

static int stage_sha256(const char *path, char output[65], char **error) {
    HfsPlusVolume stage;
    if (hfsplus_scan(path, false, &stage, error) != 0) return -1;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char *buffer = ld_xmalloc(HASH_CHUNK);
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        free(buffer);
        EVP_MD_CTX_free(context);
        hfsplus_close(&stage);
        hfsplus_set_error(error, "initialising HFS+ stage SHA-256 failed");
        return -1;
    }
    int rc = 0;
    uint32_t block = 0;
    while (block < stage.total_blocks && rc == 0) {
        if (!stage.used_map[block]) {
            ++block;
            continue;
        }
        uint32_t start = block++;
        while (block < stage.total_blocks && stage.used_map[block]) ++block;
        uint64_t offset = (uint64_t)start * stage.block_size;
        uint64_t length = (uint64_t)(block - start) * stage.block_size;
        rc = hash_update_region(context, stage.fd, offset, length,
                                buffer, HASH_CHUNK, error);
    }
    if (rc == 0)
        rc = hash_update_region(context, stage.fd, 0U, 1536U, buffer, HASH_CHUNK, error);
    if (rc == 0 && stage.bytes >= 1024U)
        rc = hash_update_region(context, stage.fd, stage.bytes - 1024U, 1024U,
                                buffer, HASH_CHUNK, error);
    if (rc == 0) rc = digest_final_hex(context, output, error);
    free(buffer);
    EVP_MD_CTX_free(context);
    hfsplus_close(&stage);
    return rc;
}

static int capture_target(const char *device, HfsPlusJournal *state, char **error) {
    if (ld_device_capture_binding(device, &state->device,
                                  &state->target_identity,
                                  &state->physical_bytes) != 0) {
        hfsplus_set_error(error, "cannot bind HFS+ target: %s", strerror(errno));
        return -1;
    }
    return volume_token(state->device, state->physical_bytes,
                        &state->signature, &state->version,
                        &state->block_size, &state->total_blocks,
                        &state->filesystem_bytes, state->volume_token, error);
}

static int check_unchanged_target(const char *device,
                                  const HfsPlusJournal *state, char **error) {
    char *canonical = NULL;
    char *identity = NULL;
    uint64_t physical_bytes = 0;
    int rc = ld_device_capture_binding(
        device, &canonical, &identity, &physical_bytes);
    if (rc != 0) {
        hfsplus_set_error(error, "cannot rebind HFS+ target: %s", strerror(errno));
        return -1;
    }
    if (strcmp(canonical, state->device) != 0 ||
        strcmp(identity, state->target_identity) != 0 ||
        physical_bytes != state->physical_bytes) {
        hfsplus_set_error(error, "HFS+ target path, identity or capacity changed before source commit");
        free(identity);
        free(canonical);
        return -1;
    }
    uint16_t signature = 0, version = 0;
    uint32_t block_size = 0, total_blocks = 0;
    uint64_t filesystem_bytes = 0;
    char token[65];
    rc = volume_token(canonical, physical_bytes, &signature, &version,
                      &block_size, &total_blocks, &filesystem_bytes, token, error);
    free(identity);
    free(canonical);
    if (rc != 0) return -1;
    if (signature != state->signature || version != state->version ||
        block_size != state->block_size || total_blocks != state->total_blocks ||
        filesystem_bytes != state->filesystem_bytes ||
        strcmp(token, state->volume_token) != 0) {
        hfsplus_set_error(error, "HFS+ filesystem identity changed before source commit");
        return -1;
    }
    return 0;
}

static int stop_commit(int target, char **error) {
    if (fsync(target) != 0) {
        hfsplus_set_error(error, "cannot sync HFS+ source at Stop boundary: %s", strerror(errno));
        return -1;
    }
    return STOPPED;
}

static int safe_commit_stage(const char *stage_path, const char *target_path,
                             const HfsPlusJournal *state,
                             uint64_t *written, char **error) {
    HfsPlusVolume stage;
    if (hfsplus_scan(stage_path, false, &stage, error) != 0) return -1;
    int target = ld_device_open_verified_fd(target_path, true,
                                            state->target_identity,
                                            state->physical_bytes);
    if (target < 0) {
        hfsplus_set_error(error, "cannot open HFS+ source for commit: %s", strerror(errno));
        hfsplus_close(&stage);
        return -1;
    }
    if (flock(target, LOCK_EX | LOCK_NB) != 0) {
        hfsplus_set_error(error, "cannot lock HFS+ source for commit: %s", strerror(errno));
        close(target);
        hfsplus_close(&stage);
        return -1;
    }
    unsigned char *buffer = ld_xmalloc(COMMIT_CHUNK);
    uint64_t total_written = 0;
    int rc = 0;
    if (ld_stop_requested()) rc = STOPPED;
    uint32_t block = 0;
    while (block < stage.total_blocks && rc == 0) {
        if (!stage.used_map[block]) {
            ++block;
            continue;
        }
        uint32_t start = block++;
        while (block < stage.total_blocks && stage.used_map[block]) ++block;
        uint64_t offset = (uint64_t)start * stage.block_size;
        uint64_t remain = (uint64_t)(block - start) * stage.block_size;
        uint64_t cursor = 0;
        while (cursor < remain) {
            if (ld_stop_requested()) {
                rc = stop_commit(target, error);
                break;
            }
            size_t take = COMMIT_CHUNK;
            if ((uint64_t)take > remain - cursor) take = (size_t)(remain - cursor);
            ssize_t got = ld_pread_full(stage.fd, buffer, take, offset + cursor);
            ssize_t put = got == (ssize_t)take
                ? ld_pwrite_full(target, buffer, take, offset + cursor) : -1;
            if (got != (ssize_t)take || put != (ssize_t)take) {
                hfsplus_set_error(error, "short I/O during HFS+ source commit");
                rc = -1;
                break;
            }
            cursor += take;
            total_written += take;
        }
    }
    if (rc == 0 && ld_stop_requested()) rc = stop_commit(target, error);
    if (rc == 0) {
        ssize_t got = ld_pread_full(stage.fd, buffer, 1536U, 0U);
        ssize_t put = got == 1536 ? ld_pwrite_full(target, buffer, 1536U, 0U) : -1;
        if (got != 1536 || put != 1536) {
            hfsplus_set_error(error, "cannot commit HFS+ primary volume-header region");
            rc = -1;
        }
    }
    if (rc == 0 && ld_stop_requested()) rc = stop_commit(target, error);
    if (rc == 0 && stage.bytes >= 1024U) {
        ssize_t got = ld_pread_full(stage.fd, buffer, 1024U, stage.bytes - 1024U);
        ssize_t put = got == 1024
            ? ld_pwrite_full(target, buffer, 1024U, stage.bytes - 1024U) : -1;
        if (got != 1024 || put != 1024) {
            hfsplus_set_error(error, "cannot commit HFS+ alternate volume-header region");
            rc = -1;
        }
    }
    if (rc == 0 && fsync(target) != 0) {
        hfsplus_set_error(error, "cannot sync HFS+ source: %s", strerror(errno));
        rc = -1;
    }
    free(buffer);
    (void)flock(target, LOCK_UN);
    close(target);
    hfsplus_close(&stage);
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
    HfsPlusJournal state;
    if (journal_load(journal, &state, error) != 0) return 1;
    if (!ld_path_is_derived_from(state.stage, journal, ".hfsplus-stage")) {
        hfsplus_set_error(error,
            "HFS+ recovery stage is not derived from the selected journal path");
        journal_free(&state);
        return 1;
    }
    if (!valid_operation(state.operation) || !valid_phase(state.phase)) {
        hfsplus_set_error(error, "HFS+ recovery journal contains an unsupported operation or phase");
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
            hfsplus_set_error(error, "HFS+ recovery stage SHA-256 does not match the persisted transaction");
        journal_free(&state);
        return 1;
    }
    bool growth = strcmp(state.operation, "growth-defrag") == 0;
    if (hfsplus_verify_layout(state.stage, growth, 10U, error) != 0) {
        journal_free(&state);
        return 1;
    }
    if (strcmp(state.phase, "committed") == 0) {
        if (hfsplus_verify_layout(device, growth, 10U, error) != 0) {
            journal_free(&state);
            return 1;
        }
        transaction_cleanup(journal, &state);
        ld_emit_result_event(stdout, "recover", "completed", "Verified an already committed HFS+ transaction.");
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
        printf("HFS+ recovery stopped at a durable source-write boundary; journal and verified stage were retained.\n");
        ld_emit_result_event(stdout, "recover", "stopped", "Recovery can be resumed safely.");
        journal_free(&state);
        return STOPPED;
    }
    if (commit_rc != 0 || hfsplus_verify_layout(device, growth, 10U, error) != 0) {
        journal_free(&state);
        return 1;
    }
    if (journal_phase(journal, &state, "committed", error) != 0) {
        journal_free(&state);
        return 1;
    }
    transaction_cleanup(journal, &state);
    if (live) {
        printf("@@LIVE_RESET {\"reason\":\"authoritative post-recovery HFS+ map\"}\n");
        fflush(stdout);
    }
    printf("Recovered verified HFS+ source; committed %" PRIu64 " KiB of allocated blocks.\n",
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
        uint16_t signature = 0, version = 0;
        uint32_t attributes = 0;
        if (hfsplus_identify(device, &signature, &version, &attributes, &error) != 0) {
            free(error);
            return 1;
        }
        printf("{\"filesystem\":\"hfsplus\",\"variant\":\"%s\",\"journaled\":%s}\n",
               signature == 0x4858U ? "HFSX" : "HFS+",
               (attributes & 0x2000U) != 0U ? "true" : "false");
        return 0;
    }
    if (strcmp(mode, "analyse-json") == 0) {
        int rc = hfsplus_analyse_json(device, &error);
        if (rc != 0) {
            fprintf(stderr, "%s\n", error == NULL ? "HFS+ analysis failed" : error);
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
            fprintf(stderr, "unknown or incomplete HFS+ option: %s\n", argv[i]);
            return 2;
        }
    }
    if (!write || journal == NULL || confirm == NULL || strcmp(confirm, device) != 0) {
        fprintf(stderr, "HFS+ mutation requires --write --confirm DEVICE --journal PATH\n");
        return 2;
    }
    if (growth && growth_percent != 10U) {
        fprintf(stderr, "HFS+ Growth Defrag requires exactly 10 percent reserve\n");
        return 2;
    }
    if (ld_path_is_mounted(device)) {
        fprintf(stderr,
            "HFS+ target is mounted; raw mutation and recovery require an unmounted filesystem\n");
        return 1;
    }

    ld_stop_clear();
    ld_stop_install_handlers();
    if (recover) {
        int rc = handle_recovery(device, journal, live, &error);
        if (rc != 0 && rc != STOPPED)
            fprintf(stderr, "%s\n", error == NULL ? "HFS+ recovery failed" : error);
        free(error);
        return rc;
    }

    HfsPlusJournal state;
    memset(&state, 0, sizeof(state));
    infiltratr_copy_string(state.operation, sizeof(state.operation), mode);
    if (capture_target(device, &state, &error) != 0) goto fail;
    state.stage = stage_name(journal);
    if (state.stage == NULL) {
        hfsplus_set_error(&error, "out of memory creating HFS+ stage path");
        goto fail;
    }
    if (access(journal, F_OK) == 0 || access(state.stage, F_OK) == 0) {
        hfsplus_set_error(&error,
            "existing HFS+ recovery artifacts must be recovered or removed before starting a new transaction");
        goto fail;
    }

    printf("Starting native C HFS+ %s on %s.\n",
           growth ? "Growth Defrag" : "Defrag", device);
    uint64_t planned = 0;
    if (hfsplus_build_stage(device, state.stage, growth, growth_percent,
                            live, &planned, &error) != 0 ||
        hfsplus_verify_layout(state.stage, growth, growth_percent, &error) != 0) {
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
    printf("HFS+ source commit: writing %" PRIu64 " KiB of verified allocated blocks only.\n",
           planned / 1024U);
    fflush(stdout);
    uint64_t written = 0;
    int commit_rc = safe_commit_stage(state.stage, device, &state, &written, &error);
    if (commit_rc == STOPPED) {
        printf("HFS+ Stop reached a durable source-write boundary; recovery journal and verified stage were retained.\n");
        ld_emit_result_event(stdout, mode, "stopped", "Run Recover to resume the verified transaction.");
        journal_free(&state);
        free(error);
        return STOPPED;
    }
    if (commit_rc != 0 || hfsplus_verify_layout(device, growth, growth_percent, &error) != 0)
        goto fail;
    if (journal_phase(journal, &state, "committed", &error) != 0) goto fail;
    transaction_cleanup(journal, &state);
    if (live) {
        printf("@@LIVE_RESET {\"reason\":\"authoritative post-commit HFS+ map\"}\n");
        fflush(stdout);
    }
    printf("HFS+ %s completed; committed %" PRIu64 " KiB of allocated blocks.\n",
           growth ? "Growth Defrag" : "Defrag", written / 1024U);
    ld_emit_result_event(stdout, mode, "completed", "");
    journal_free(&state);
    free(error);
    return 0;

fail:
    fprintf(stderr, "%s\n", error == NULL ? "HFS+ transaction failed" : error);
    free(error);
    journal_free(&state);
    return 1;
}
