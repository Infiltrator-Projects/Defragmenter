// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "test_media_raw.h"
#include "ld_device.h"
#include "ld_io.h"

#include "infiltratr/arithmetic.h"
#include "infiltratr/core.h"
#include "infiltratr/endian.h"
#include "infiltratr/posix_path.h"
#include "infiltratr/posix.h"
#include "infiltratr/posix_io.h"
#include "infiltratr/token.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LDTM_MAX_TARGET_FILES LDTM_RAW_FILES
#define LDTM_HASH_HEX 65U
#define LDTM_LINE_MAX 4096U
#define LDTM_CAPTURE_MAX (8U * 1024U * 1024U)
#define LDTM_PROCESS_TIMEOUT_MS UINT64_C(600000)
#define LDTM_PROCESS_POLL_MS 100

static const uint32_t ldtm_edge_case_sizes[] = {
    0U, 1U, 511U, 512U, 513U, 4095U, 4096U, 4097U
};
#define LDTM_EDGE_CASE_COUNT \
    (sizeof(ldtm_edge_case_sizes) / sizeof(ldtm_edge_case_sizes[0]))

typedef struct {
    uint64_t size;
    char sha256[LDTM_HASH_HEX];
} LdtmTargetRecord;

typedef struct {
    char path[PATH_MAX];
    char label[64];
} LdtmPartition;

typedef struct {
    LdtmPartition items[LDTM_SPEC_COUNT];
    size_t count;
} LdtmPartitionMap;

static int load_partition_map(const char *device, LdtmPartitionMap *map);

typedef struct {
    int populated;
    char build_status[64];
    char build_detail[512];
} LdtmVerifyFilesystem;

static void digest_to_hex(const unsigned char *digest, unsigned int length,
                          char output[LDTM_HASH_HEX]);

static void emit_status(const char *filesystem, const char *status, const char *detail) {
    const char *safe_detail = detail != NULL ? detail : "";
    printf("LDTM_STATUS\t%s\t%s\t%s\n", filesystem, status, safe_detail);
    fflush(stdout);
}

static int monotonic_millis(uint64_t *value)
{
    struct timespec now;
    if (value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    if (now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000)) {
        errno = EOVERFLOW;
        return -1;
    }
    *value = (uint64_t)now.tv_sec * UINT64_C(1000) +
             (uint64_t)now.tv_nsec / UINT64_C(1000000);
    return 0;
}

static void terminate_child_process_group(pid_t child)
{
    if (child <= 0)
        return;
    if (kill(-child, SIGKILL) != 0 && errno == ESRCH)
        (void)kill(child, SIGKILL);
}

static void reap_child(pid_t child, int *status)
{
    int local_status = 0;
    if (status == NULL)
        status = &local_status;
    while (waitpid(child, status, 0) < 0 && errno == EINTR) {}
}

static int wait_child_until(pid_t child, int *status, uint64_t started_ms)
{
    const struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 50L * 1000L * 1000L
    };
    for (;;) {
        const pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child)
            return 0;
        if (waited < 0 && errno != EINTR)
            return -1;

        uint64_t now_ms = 0U;
        if (monotonic_millis(&now_ms) != 0) {
            const int failure = errno;
            terminate_child_process_group(child);
            reap_child(child, status);
            errno = failure;
            return -1;
        }
        if (now_ms - started_ms >= LDTM_PROCESS_TIMEOUT_MS) {
            terminate_child_process_group(child);
            reap_child(child, status);
            errno = ETIMEDOUT;
            return -1;
        }
        (void)nanosleep(&delay, NULL);
    }
}

static int run_process(const char *const argv[], const char *stdin_text, int quiet)
{
    int input_pipe[2] = {-1, -1};
    pid_t child;
    int status = 0;
    char program[PATH_MAX];
    uint64_t started_ms = 0U;

    if (argv == NULL || argv[0] == NULL ||
        ldtm_resolve_program(argv[0], program, sizeof(program)) != 0)
        return -1;
    if (monotonic_millis(&started_ms) != 0)
        return -1;

    if (!quiet) {
        size_t index = 0U;
        fputs("+", stdout);
        while (argv[index] != NULL) {
            printf(" %s", argv[index]);
            ++index;
        }
        fputc('\n', stdout);
        fflush(stdout);
    }

    if (stdin_text != NULL && pipe(input_pipe) != 0)
        return -1;
    child = fork();
    if (child < 0) {
        if (input_pipe[0] >= 0) {
            (void)close(input_pipe[0]);
            (void)close(input_pipe[1]);
        }
        return -1;
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0)
            _exit(126);
        if (stdin_text != NULL) {
            (void)close(input_pipe[1]);
            if (dup2(input_pipe[0], STDIN_FILENO) < 0)
                _exit(126);
            (void)close(input_pipe[0]);
        }
        execv(program, (char *const *)argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    (void)setpgid(child, child);
    if (stdin_text != NULL) {
        const size_t length = strlen(stdin_text);
        (void)close(input_pipe[0]);
        if (infiltratr_write_full(input_pipe[1], stdin_text, length) != 0) {
            const int failure = errno;
            (void)close(input_pipe[1]);
            terminate_child_process_group(child);
            reap_child(child, &status);
            errno = failure;
            return -1;
        }
        (void)close(input_pipe[1]);
    }

    if (wait_child_until(child, &status, started_ms) != 0) {
        if (errno == ETIMEDOUT)
            (void)fprintf(stderr,
                          "test-media: command timed out after 600 seconds: %s\n",
                          argv[0]);
        return -1;
    }
    if (!WIFEXITED(status)) {
        errno = ECHILD;
        return -1;
    }
    if (WEXITSTATUS(status) != 0 && !quiet)
        (void)fprintf(stderr, "test-media: command failed (%d): %s\n",
                      WEXITSTATUS(status), argv[0]);
    return WEXITSTATUS(status);
}

static int capture_process(const char *const argv[], char **output)
{
    int output_pipe[2];
    pid_t child;
    int status = 0;
    size_t capacity = 0U;
    size_t used = 0U;
    char *buffer = NULL;
    char program[PATH_MAX];
    uint64_t started_ms = 0U;

    if (output == NULL || argv == NULL || argv[0] == NULL ||
        ldtm_resolve_program(argv[0], program, sizeof(program)) != 0)
        return -1;
    *output = NULL;
    if (monotonic_millis(&started_ms) != 0)
        return -1;
    if (pipe(output_pipe) != 0)
        return -1;

    child = fork();
    if (child < 0) {
        (void)close(output_pipe[0]);
        (void)close(output_pipe[1]);
        return -1;
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0)
            _exit(126);
        (void)close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(output_pipe[1]);
        execv(program, (char *const *)argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    (void)setpgid(child, child);
    (void)close(output_pipe[1]);

    const int current_flags = fcntl(output_pipe[0], F_GETFL);
    if (current_flags < 0 ||
        fcntl(output_pipe[0], F_SETFL, current_flags | O_NONBLOCK) != 0) {
        const int failure = errno;
        (void)close(output_pipe[0]);
        terminate_child_process_group(child);
        reap_child(child, &status);
        errno = failure;
        return -1;
    }

    if (!infiltratr_array_reserve((void **)&buffer, &capacity, 1U,
                                  4096U, 4096U)) {
        (void)close(output_pipe[0]);
        terminate_child_process_group(child);
        reap_child(child, &status);
        return -1;
    }

    bool eof = false;
    while (!eof) {
        if (used == LDTM_CAPTURE_MAX) {
            unsigned char extra = 0U;
            const ssize_t probe = read(output_pipe[0], &extra, 1U);
            if (probe == 0) {
                eof = true;
                break;
            }
            if (probe < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                /* No extra byte is currently buffered; poll drives progress. */
            } else {
                const int failure = probe < 0 ? errno : EOVERFLOW;
                free(buffer);
                (void)close(output_pipe[0]);
                terminate_child_process_group(child);
                reap_child(child, &status);
                errno = failure;
                return -1;
            }
        } else {
            const size_t remaining_limit = LDTM_CAPTURE_MAX - used;
            if (capacity - used < 2048U) {
                size_t required = 0U;
                const size_t wanted = remaining_limit < 2048U
                    ? remaining_limit + 1U : 2049U;
                if (!infiltratr_size_add_checked(used, wanted, &required) ||
                    required > LDTM_CAPTURE_MAX + 1U ||
                    !infiltratr_array_reserve((void **)&buffer, &capacity, 1U,
                                              required, 4096U)) {
                    free(buffer);
                    (void)close(output_pipe[0]);
                    terminate_child_process_group(child);
                    reap_child(child, &status);
                    return -1;
                }
            }

            size_t readable = capacity - used - 1U;
            if (readable > remaining_limit)
                readable = remaining_limit;
            const ssize_t got = read(output_pipe[0], buffer + used, readable);
            if (got > 0) {
                used += (size_t)got;
                continue;
            }
            if (got == 0) {
                eof = true;
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                const int failure = errno;
                free(buffer);
                (void)close(output_pipe[0]);
                terminate_child_process_group(child);
                reap_child(child, &status);
                errno = failure;
                return -1;
            }
        }

        uint64_t now_ms = 0U;
        if (monotonic_millis(&now_ms) != 0) {
            const int failure = errno;
            free(buffer);
            (void)close(output_pipe[0]);
            terminate_child_process_group(child);
            reap_child(child, &status);
            errno = failure;
            return -1;
        }
        if (now_ms - started_ms >= LDTM_PROCESS_TIMEOUT_MS) {
            free(buffer);
            (void)close(output_pipe[0]);
            terminate_child_process_group(child);
            reap_child(child, &status);
            errno = ETIMEDOUT;
            (void)fprintf(stderr,
                          "test-media: command timed out after 600 seconds: %s\n",
                          argv[0]);
            return -1;
        }

        struct pollfd descriptor = {
            .fd = output_pipe[0],
            .events = POLLIN | POLLHUP,
            .revents = 0
        };
        int poll_result;
        do {
            poll_result = poll(&descriptor, (nfds_t)1, LDTM_PROCESS_POLL_MS);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            const int failure = errno;
            free(buffer);
            (void)close(output_pipe[0]);
            terminate_child_process_group(child);
            reap_child(child, &status);
            errno = failure;
            return -1;
        }
    }

    (void)close(output_pipe[0]);
    if (wait_child_until(child, &status, started_ms) != 0) {
        const int failure = errno;
        free(buffer);
        if (failure == ETIMEDOUT)
            (void)fprintf(stderr,
                          "test-media: command timed out after 600 seconds: %s\n",
                          argv[0]);
        errno = failure;
        return -1;
    }

    buffer[used] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status))
            (void)fprintf(stderr, "test-media: command failed (%d): %s\n",
                          WEXITSTATUS(status), argv[0]);
        free(buffer);
        errno = EIO;
        return -1;
    }
    *output = buffer;
    return 0;
}

static const char *production_mapper_program(void) {
    const char *configured = getenv("DEFRAGMENTER_MAPPER");
    if (configured != NULL && *configured != '\0' &&
        access(configured, X_OK) == 0)
        return configured;
    if (access("/System/Commands/Defragmenter/defragmenter-mapper", X_OK) == 0)
        return "/System/Commands/Defragmenter/defragmenter-mapper";
    if (ldtm_program_available("defragmenter-mapper"))
        return "defragmenter-mapper";
    return NULL;
}

static const char *json_skip_ws(const char *cursor) {
    while (*cursor != '\0' && isspace((unsigned char)*cursor))
        ++cursor;
    return cursor;
}

static int json_scan_string(const char **cursor, const char **start,
                            size_t *length, int *escaped) {
    const char *p = *cursor;
    if (*p != '"') return -1;
    ++p;
    const char *begin = p;
    int has_escape = 0;
    while (*p != '\0') {
        const unsigned char byte = (unsigned char)*p;
        if (byte == '"') {
            if (start != NULL) *start = begin;
            if (length != NULL) *length = (size_t)(p - begin);
            if (escaped != NULL) *escaped = has_escape;
            *cursor = p + 1;
            return 0;
        }
        if (byte < 0x20U) return -1;
        if (byte == '\\') {
            has_escape = 1;
            ++p;
            if (*p == '\0') return -1;
            if (*p == 'u') {
                for (unsigned int digit = 0U; digit < 4U; ++digit) {
                    ++p;
                    if (!isxdigit((unsigned char)*p)) return -1;
                }
            } else if (strchr("\"\\/bfnrt", *p) == NULL) {
                return -1;
            }
        }
        ++p;
    }
    return -1;
}

static int json_skip_value(const char **cursor, unsigned int depth) {
    if (depth > 64U) return -1;
    const char *p = json_skip_ws(*cursor);
    if (*p == '"') {
        if (json_scan_string(&p, NULL, NULL, NULL) != 0) return -1;
    } else if (*p == '{') {
        ++p;
        p = json_skip_ws(p);
        if (*p == '}') {
            ++p;
        } else {
            for (;;) {
                if (json_scan_string(&p, NULL, NULL, NULL) != 0) return -1;
                p = json_skip_ws(p);
                if (*p++ != ':') return -1;
                if (json_skip_value(&p, depth + 1U) != 0) return -1;
                p = json_skip_ws(p);
                if (*p == '}') {
                    ++p;
                    break;
                }
                if (*p++ != ',') return -1;
                p = json_skip_ws(p);
            }
        }
    } else if (*p == '[') {
        ++p;
        p = json_skip_ws(p);
        if (*p == ']') {
            ++p;
        } else {
            for (;;) {
                if (json_skip_value(&p, depth + 1U) != 0) return -1;
                p = json_skip_ws(p);
                if (*p == ']') {
                    ++p;
                    break;
                }
                if (*p++ != ',') return -1;
            }
        }
    } else if (strncmp(p, "true", 4U) == 0) {
        p += 4;
    } else if (strncmp(p, "false", 5U) == 0) {
        p += 5;
    } else if (strncmp(p, "null", 4U) == 0) {
        p += 4;
    } else {
        const char *number = p;
        if (*p == '-') ++p;
        if (*p == '0') {
            ++p;
            if (isdigit((unsigned char)*p)) return -1;
        } else {
            if (*p < '1' || *p > '9') return -1;
            while (isdigit((unsigned char)*p)) ++p;
        }
        if (*p == '.') {
            ++p;
            if (!isdigit((unsigned char)*p)) return -1;
            while (isdigit((unsigned char)*p)) ++p;
        }
        if (*p == 'e' || *p == 'E') {
            ++p;
            if (*p == '+' || *p == '-') ++p;
            if (!isdigit((unsigned char)*p)) return -1;
            while (isdigit((unsigned char)*p)) ++p;
        }
        if (p == number) return -1;
    }
    *cursor = p;
    return 0;
}

static int parse_json_u64_field(const char *json, const char *name,
                                uint64_t *value) {
    if (json == NULL || name == NULL || value == NULL) return -1;
    const size_t wanted_length = strlen(name);
    const char *cursor = json_skip_ws(json);
    uint64_t parsed_value = 0U;
    int found = 0;

    if (*cursor++ != '{') return -1;
    cursor = json_skip_ws(cursor);

    while (*cursor != '\0' && *cursor != '}') {
        const char *key = NULL;
        size_t key_length = 0U;
        int escaped = 0;
        if (json_scan_string(&cursor, &key, &key_length, &escaped) != 0)
            return -1;
        cursor = json_skip_ws(cursor);
        if (*cursor++ != ':') return -1;
        cursor = json_skip_ws(cursor);

        const int matches = escaped == 0 &&
            key_length == wanted_length &&
            memcmp(key, name, wanted_length) == 0;
        if (matches) {
            if (found || *cursor < '0' || *cursor > '9') return -1;
            const char *number = cursor;
            if (!infiltratr_parse_u64_token(&cursor, 10U, &parsed_value))
                return -1;
            if (cursor == number ||
                (*number == '0' && cursor - number > 1))
                return -1;
            found = 1;
        } else if (json_skip_value(&cursor, 0U) != 0) {
            return -1;
        }

        cursor = json_skip_ws(cursor);
        if (*cursor == '}') break;
        if (*cursor++ != ',') return -1;
        cursor = json_skip_ws(cursor);
    }

    if (*cursor++ != '}') return -1;
    cursor = json_skip_ws(cursor);
    if (*cursor != '\0' || !found) return -1;
    *value = parsed_value;
    return 0;
}

static int production_map_output(const LdtmFilesystemSpec *spec,
                                 const char *partition,
                                 char **output,
                                 char *detail, size_t detail_capacity) {
    const char *program;
    if (output == NULL || spec == NULL || partition == NULL ||
        detail == NULL || detail_capacity == 0U)
        return -1;
    *output = NULL;
    detail[0] = '\0';
    program = production_mapper_program();
    if (program == NULL) {
        (void)snprintf(detail, detail_capacity,
                       "production allocation mapper is unavailable");
        return -1;
    }
    const char *const argv[] = {
        program, partition, "--fstype", spec->key, "--cells", "1", NULL
    };
    if (capture_process(argv, output) != 0 || *output == NULL) {
        free(*output);
        *output = NULL;
        (void)snprintf(detail, detail_capacity,
                       "production analyser rejected the generated filesystem");
        return -1;
    }
    return 0;
}

static int production_fragment_counts(const LdtmFilesystemSpec *spec,
                                      const char *partition,
                                      uint64_t *fragmented_files,
                                      uint64_t *fragmented_directories,
                                      char *detail,
                                      size_t detail_capacity) {
    char *output = NULL;
    if (fragmented_files == NULL || fragmented_directories == NULL)
        return -1;
    *fragmented_files = 0U;
    *fragmented_directories = 0U;
    if (production_map_output(spec, partition, &output,
                              detail, detail_capacity) != 0)
        return -1;
    if (parse_json_u64_field(
            output, "fragmented_files", fragmented_files) != 0 ||
        parse_json_u64_field(
            output, "fragmented_directories",
            fragmented_directories) != 0) {
        free(output);
        (void)snprintf(
            detail, detail_capacity,
            "production analyser did not report complete fragmentation totals");
        return -1;
    }
    free(output);
    (void)snprintf(
        detail, detail_capacity,
        "production analyser reports %llu fragmented file%s and %llu fragmented director%s",
        (unsigned long long)*fragmented_files,
        *fragmented_files == 1U ? "" : "s",
        (unsigned long long)*fragmented_directories,
        *fragmented_directories == 1U ? "y" : "ies");
    return 0;
}

static int production_map_accepts(const LdtmFilesystemSpec *spec,
                                  const char *partition,
                                  char *detail, size_t detail_capacity) {
    char *output = NULL;
    if (production_map_output(spec, partition, &output,
                              detail, detail_capacity) != 0)
        return -1;
    free(output);
    (void)snprintf(
        detail, detail_capacity,
        "production mapper accepted the complete filesystem geometry");
    return 0;
}

static int require_fragmentation_state(const LdtmFilesystemSpec *spec,
                                       const char *partition,
                                       int expect_fragmented,
                                       char *detail, size_t detail_capacity) {
    uint64_t fragmented_files = 0U;
    uint64_t fragmented_directories = 0U;
    if (production_fragment_counts(
            spec, partition, &fragmented_files,
            &fragmented_directories, detail, detail_capacity) != 0)
        return -1;
    if (expect_fragmented != 0 && fragmented_files == 0U) {
        (void)snprintf(
            detail, detail_capacity,
            "qualification requires a genuinely fragmented retained file before Defragment; production analyser reported zero fragmented files");
        return -1;
    }
    if (expect_fragmented == 0 &&
        (fragmented_files != 0U || fragmented_directories != 0U)) {
        (void)snprintf(
            detail, detail_capacity,
            "post-Defragment qualification requires zero fragmented files and directories; production analyser saw %llu file%s and %llu director%s",
            (unsigned long long)fragmented_files,
            fragmented_files == 1U ? "" : "s",
            (unsigned long long)fragmented_directories,
            fragmented_directories == 1U ? "y" : "ies");
        return -1;
    }
    return 0;
}

static int extract_pair(const char *line, const char *key, char *value, size_t capacity) {
    char needle[64];
    const char *found;
    const char *cursor;
    size_t used = 0U;
    if (line == NULL || key == NULL || value == NULL || capacity == 0U) return 0;
    if (snprintf(needle, sizeof(needle), "%s=\"", key) <= 0) return 0;
    found = strstr(line, needle);
    if (found == NULL) return 0;
    cursor = found + strlen(needle);
    while (*cursor != '\0' && *cursor != '"' && used + 1U < capacity) {
        unsigned char decoded = 0U;
        if (cursor[0] == '\\' && cursor[1] == 'x' &&
            cursor[2] != '\0' && cursor[3] != '\0' &&
            ldtm_decode_hex_byte(cursor[2], cursor[3], &decoded)) {
            value[used++] = (char)decoded;
            cursor += 4;
        } else if (cursor[0] == '\\' && cursor[1] != '\0') {
            value[used++] = cursor[1];
            cursor += 2;
        } else {
            value[used++] = *cursor++;
        }
    }
    value[used] = '\0';
    return 1;
}

int ldtm_device_fingerprint(const char *device, char output[65]) {
    char canonical[PATH_MAX];
    char material[1024];
    LdBlockDeviceInfo info;
    EVP_MD_CTX *context = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0U;
    int result = -1;

    if (device == NULL || output == NULL ||
        ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0 ||
        ld_block_device_info(canonical, &info) != 0 ||
        !info.whole_disk || info.size_bytes == 0U)
        return -1;

    /*
     * A destructive confirmation must identify a physical medium, not merely
     * a model/capacity class. Two anonymous devices can legitimately have the
     * same geometry, so serial or WWN remains mandatory.
     */
    if (info.serial[0] == '\0' && info.wwn[0] == '\0')
        return -1;

    const int length = snprintf(
        material, sizeof(material),
        "size=%llu\nmodel=%s\nserial=%s\nwwn=%s\ntransport=%s\n",
        (unsigned long long)info.size_bytes, info.model, info.serial,
        info.wwn, info.transport);
    if (length < 0 || (size_t)length >= sizeof(material))
        return -1;

    context = EVP_MD_CTX_new();
    if (context == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(context, material, (size_t)length) != 1 ||
        EVP_DigestFinal_ex(context, digest, &digest_length) != 1)
        goto cleanup;
    digest_to_hex(digest, digest_length, output);
    result = 0;
cleanup:
    if (context != NULL) EVP_MD_CTX_free(context);
    return result;
}

int ldtm_is_whole_block_device(const char *device) {
    char canonical[PATH_MAX];
    LdBlockDeviceInfo info;
    return device != NULL &&
           ldtm_canonicalize_device(device, canonical, sizeof(canonical)) == 0 &&
           ld_block_device_info(canonical, &info) == 0 &&
           info.whole_disk;
}

int ldtm_is_system_disk(const char *device) {
    char canonical[PATH_MAX];
    bool in_use = true;
    if (device == NULL ||
        ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0 ||
        ld_block_device_has_system_use(canonical, &in_use) != 0)
        return 1;
    return in_use ? 1 : 0;
}

int ldtm_device_safety_check(const char *device, int allow_non_removable,
                             char *detail, size_t detail_capacity) {
    char canonical[PATH_MAX];
    LdBlockDeviceInfo info;
    bool system_use = true;

    if (detail == NULL || detail_capacity == 0U) return -1;
    detail[0] = '\0';
    if (device == NULL ||
        ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0) {
        (void)snprintf(detail, detail_capacity, "Target does not exist: %s",
                       device != NULL ? device : "(null)");
        return -1;
    }
    if (ld_block_device_info(canonical, &info) != 0) {
        (void)snprintf(detail, detail_capacity, "Unable to inspect %s", canonical);
        return -1;
    }
    if (!info.whole_disk) {
        (void)snprintf(detail, detail_capacity,
                       "Refusing non-whole-disk target: %s", canonical);
        return -1;
    }
    if (ld_block_device_has_system_use(canonical, &system_use) != 0 ||
        system_use) {
        (void)snprintf(detail, detail_capacity,
                       "Refusing disk backing active system storage: %s",
                       canonical);
        return -1;
    }
    if (info.read_only) {
        (void)snprintf(detail, detail_capacity,
                       "Refusing read-only target: %s", canonical);
        return -1;
    }
    if (info.size_bytes < ldtm_required_capacity_bytes()) {
        (void)snprintf(detail, detail_capacity,
                       "Target is too small: %.1f GiB; need at least %.1f GiB",
                       (double)info.size_bytes / (double)LDTM_GIB,
                       (double)ldtm_required_capacity_bytes() / (double)LDTM_GIB);
        return -1;
    }
    if (!allow_non_removable &&
        !ldtm_transport_is_field_media(info.removable ? 1 : 0,
                                       info.transport)) {
        (void)snprintf(detail, detail_capacity,
                       "Refusing non-removable target by default (RM=%d, TRAN=%s)",
                       info.removable ? 1 : 0,
                       info.transport[0] != '\0' ? info.transport : "unknown");
        return -1;
    }
    (void)snprintf(detail, detail_capacity,
                   "Safe field-media target: %s", canonical);
    return 0;
}

static int unmount_descendants(const char *device) {
    const char *const argv[] = {"lsblk", "-n", "-p", "-r", "-o", "MOUNTPOINT", "--", device, NULL};
    char *output = NULL;
    char *line;
    char *saveptr = NULL;
    int result = 0;
    if (capture_process(argv, &output) != 0) return -1;
    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        infiltratr_trim(line);
        if (*line != '\0') {
            const char *const umount_argv[] = {"umount", line, NULL};
            if (run_process(umount_argv, NULL, 0) != 0) result = -1;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(output);
    return result;
}

static int settle_partitions(const char *device, LdtmPartitionMap *map) {
    const char *const partx_argv[] = {"partx", "-u", device, NULL};
    const char *const udev_argv[] = {"udevadm", "settle", NULL};
    unsigned int attempt;

    if (map == NULL || run_process(partx_argv, NULL, 0) != 0)
        return -1;
    if (ldtm_program_available("udevadm") &&
        run_process(udev_argv, NULL, 0) != 0)
        return -1;

    /*
     * A fixed sleep is neither necessary nor sufficient.  Wait until the
     * kernel view actually contains the complete labelled GPT layout, with a
     * bounded five-second ceiling so a broken device can never hang the GUI.
     */
    for (attempt = 0U; attempt < 50U; ++attempt) {
        if (load_partition_map(device, map) == 0 &&
            map->count == LDTM_SPEC_COUNT)
            return 0;
        usleep(100000U);
    }
    return -1;
}

static int load_partition_map(const char *device, LdtmPartitionMap *map) {
    const char *const argv[] = {"lsblk", "-n", "-p", "-P", "-o", "PATH,PARTLABEL", "--", device, NULL};
    char *output = NULL;
    char *line;
    char *saveptr = NULL;
    if (map == NULL) return -1;
    memset(map, 0, sizeof(*map));
    if (capture_process(argv, &output) != 0) return -1;
    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL && map->count < LDTM_SPEC_COUNT) {
        char path[PATH_MAX] = "";
        char label[64] = "";
        if (extract_pair(line, "PATH", path, sizeof(path)) &&
            extract_pair(line, "PARTLABEL", label, sizeof(label)) && *label != '\0') {
            (void)snprintf(map->items[map->count].path,
                           sizeof(map->items[map->count].path), "%s", path);
            (void)snprintf(map->items[map->count].label,
                           sizeof(map->items[map->count].label), "%s", label);
            ++map->count;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(output);
    return map->count > 0U ? 0 : -1;
}

static const char *partition_for_label(const LdtmPartitionMap *map, const char *label) {
    size_t index;
    if (map == NULL || label == NULL) return NULL;
    for (index = 0U; index < map->count; ++index) {
        if (strcmp(map->items[index].label, label) == 0) return map->items[index].path;
    }
    return NULL;
}

static int ensure_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST) return 0;
    return -1;
}

static void deterministic_fill(unsigned char *buffer, size_t length, uint64_t seed) {
    uint64_t state = seed | UINT64_C(1);
    size_t index;
    for (index = 0U; index < length; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        buffer[index] = (unsigned char)(state & UINT64_C(0xff));
    }
}

static int write_pattern_file(const char *path, uint64_t size, uint64_t seed) {
    unsigned char buffer[65536];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    uint64_t remaining = size;
    uint64_t block = 0U;
    if (fd < 0) return -1;
    while (remaining > 0U) {
        const size_t chunk = remaining < (uint64_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        deterministic_fill(buffer, chunk, seed ^ block);
        if (infiltratr_write_full(fd, buffer, chunk) != 0) {
            (void)close(fd);
            return -1;
        }
        remaining -= (uint64_t)chunk;
        ++block;
    }
    if (fsync(fd) != 0) {
        (void)close(fd);
        return -1;
    }
    return close(fd);
}

static uint64_t edge_case_seed(size_t index, uint32_t size) {
    return UINT64_C(0x4544474500000000) ^
           ((uint64_t)index << 32U) ^ (uint64_t)size;
}

static int generate_edge_case_data(const char *root) {
    char directory[PATH_MAX];
    size_t index;
    if (!infiltratr_path_join(directory, sizeof(directory), root, "edge-cases") ||
        ensure_directory(directory, 0755) != 0)
        return -1;
    for (index = 0U; index < LDTM_EDGE_CASE_COUNT; ++index) {
        char path[PATH_MAX];
        char name[64];
        const uint32_t size = ldtm_edge_case_sizes[index];
        (void)snprintf(name, sizeof(name), "edge-%02zu-%u.bin", index, size);
        if (!infiltratr_path_join(path, sizeof(path), directory, name) ||
            write_pattern_file(path, size, edge_case_seed(index, size)) != 0)
            return -1;
    }
    return 0;
}

static void digest_to_hex(const unsigned char *digest, unsigned int length, char output[LDTM_HASH_HEX]) {
    unsigned int index;
    static const char hex[] = "0123456789abcdef";
    for (index = 0U; index < length && index < 32U; ++index) {
        output[index * 2U] = hex[(digest[index] >> 4) & 0x0fU];
        output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    output[64] = '\0';
}

/* The package-owned UFS2 creator consumes an ordinary source directory.
 * No UFS driver or mounted target is involved. Physical fragmentation is
 * manufactured by that creator, independently of the host source layout. */
static int generate_ufs_source(const char *source)
{
    const LdtmFilesystemSpec *spec = ldtm_find_spec("ufs");
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    char root[PATH_MAX], files[PATH_MAX], directory[PATH_MAX];
    if (!infiltratr_path_join(root, sizeof(root), source, "Defragmenter-TestData") ||
        !infiltratr_path_join(files, sizeof(files), root, "fragmented-files") ||
        !infiltratr_path_join(directory, sizeof(directory), root, "fragmented-directory") ||
        ensure_directory(root, 0755) != 0 || ensure_directory(files, 0755) != 0 ||
        ensure_directory(directory, 0755) != 0)
        return -1;
    const size_t chunk_bytes = (size_t)profile.chunk_kib * 1024U;
    uint8_t *buffer = malloc(chunk_bytes);
    if (buffer == NULL)
        return -1;
    int result = 0;
    for (unsigned file = 0U; file < LDTM_TARGET_FILE_COUNT && result == 0; ++file) {
        char path[PATH_MAX], name[64];
        (void)snprintf(name, sizeof(name), "fragmented-%02u.bin", file);
        if (!infiltratr_path_join(path, sizeof(path), files, name)) {
            result = -1;
            break;
        }
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            result = -1;
            break;
        }
        for (uint32_t chunk = 0U; result == 0 && chunk < ldtm_profile_file_chunks(&profile, file);
             ++chunk) {
            const uint64_t seed =
                ((uint64_t)file << 48U) ^ ((uint64_t)chunk << 16U) ^ UINT64_C(0x4c44544d);
            deterministic_fill(buffer, chunk_bytes, seed);
            result = infiltratr_write_full(fd, buffer, chunk_bytes);
        }
        if (fsync(fd) != 0)
            result = -1;
        if (close(fd) != 0)
            result = -1;
    }
    free(buffer);
    return result == 0 ? generate_edge_case_data(root) : -1;
}

static void sanitize_tsv(char *text) {
    if (text == NULL) return;
    while (*text != '\0') {
        if (*text == '\t' || *text == '\r' || *text == '\n') *text = ' ';
        ++text;
    }
}

static int state_path_for_device(const char *device, char *path, size_t capacity) {
    char fingerprint[LDTM_HASH_HEX];
    int count;
    if (ldtm_device_fingerprint(device, fingerprint) != 0)
        return -1;
    count = snprintf(
        path, capacity, "%s/disk-%s.tsv", LDTM_STATE_ROOT, fingerprint);
    return count < 0 || (size_t)count >= capacity ? -1 : 0;
}

static int secure_state_root_fd(void) {
    struct stat st;
    int fd;
    if (mkdir(LDTM_STATE_ROOT, 0700) != 0 && errno != EEXIST)
        return -1;
    if (lstat(LDTM_STATE_ROOT, &st) != 0 ||
        !S_ISDIR(st.st_mode) || st.st_uid != 0U) {
        errno = EPERM;
        return -1;
    }
    if ((st.st_mode & 0777U) != 0700U && chmod(LDTM_STATE_ROOT, 0700) != 0)
        return -1;
    fd = open(LDTM_STATE_ROOT,
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0U) {
        (void)close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

static FILE *open_state_stream(const char *state_path, int write_mode) {
    const char *base = infiltratr_path_basename(state_path);
    const int root_fd = secure_state_root_fd();
    int fd;
    int flags;
    struct stat st;
    FILE *stream;

    if (root_fd < 0 || base == NULL || *base == '\0' || strchr(base, '/') != NULL) {
        if (root_fd >= 0) (void)close(root_fd);
        return NULL;
    }
    flags = write_mode != 0
        ? O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW
        : O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    fd = openat(root_fd, base, flags, 0600);
    if (fd >= 0 && write_mode != 0 && ld_sync_fd(root_fd) != 0) {
        const int failure = errno;
        (void)close(fd);
        (void)close(root_fd);
        errno = failure;
        return NULL;
    }
    (void)close(root_fd);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != 0U || st.st_nlink != 1U) {
        (void)close(fd);
        errno = EPERM;
        return NULL;
    }
    if (write_mode != 0 && fchmod(fd, 0600) != 0) {
        (void)close(fd);
        return NULL;
    }
    stream = fdopen(fd, write_mode != 0 ? "w" : "r");
    if (stream == NULL)
        (void)close(fd);
    return stream;
}

static int state_flush(FILE *state) {
    if (state == NULL || fflush(state) != 0)
        return -1;
    const int fd = fileno(state);
    return fd >= 0 && ld_sync_fd(fd) == 0 ? 0 : -1;
}

static int state_write_status(FILE *state, const LdtmFilesystemSpec *spec,
                              const char *status, const char *detail) {
    char safe[512];
    if (state == NULL || spec == NULL) return -1;
    (void)snprintf(safe, sizeof(safe), "%s", detail != NULL ? detail : "");
    sanitize_tsv(safe);
    if (fprintf(state, "fs\t%s\t%s\t%s\n", spec->key, status, safe) < 0) return -1;
    return state_flush(state);
}

static int create_amiga_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                     FILE *state) {
    const char *const wipe_argv[] = {"wipefs", "--all", "--force", partition, NULL};
    const uint8_t dostype = strcmp(spec->key, "ofs") == 0 ? (uint8_t)0 : (uint8_t)1;
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    char detail[512];
    if (run_process(wipe_argv, NULL, 0) != 0) {
        emit_status(spec->key, "format-failed", "could not clear old filesystem signatures");
        (void)state_write_status(state, spec, "format-failed", "wipefs failed");
        return 1;
    }
    printf("+ built-in raw C Amiga DOS\\%u formatter/populator %s\n", dostype, partition);
    fflush(stdout);
    if (ldtm_format_amiga_volume(partition, dostype, spec->label) != 0) {
        emit_status(spec->key, "format-failed", "built-in C Amiga formatter failed");
        (void)state_write_status(state, spec, "format-failed", "built-in C formatter failed");
        return 1;
    }
    if (ldtm_populate_amiga_volume(partition, dostype, &profile) != 0) {
        emit_status(spec->key, "formatted-unpopulated", "raw C Amiga fragmentation payload generation failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "raw C payload generation failed");
        return 1;
    }
    detail[0] = '\0';
    if (ldtm_verify_amiga_payload(partition, dostype, &profile, detail, sizeof(detail)) != 0) {
        emit_status(spec->key, "formatted-unpopulated",
                    detail[0] != '\0' ? detail : "raw C Amiga payload self-check failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "raw C payload self-check failed");
        return 1;
    }
    {
        char analyser_detail[512];
        if (require_fragmentation_state(
                spec, partition, 1,
                analyser_detail, sizeof(analyser_detail)) != 0) {
            emit_status(spec->key, "qualification-failed",
                        analyser_detail);
            (void)state_write_status(
                state, spec, "qualification-failed",
                analyser_detail);
            return 1;
        }
    }
    if (state_write_status(
            state, spec, "populated",
            "raw C deterministic payload created; independent raw verifier and production analyser both prove fragmentation") != 0)
        return -1;
    emit_status(
        spec->key, "populated",
        "raw C deterministic payload created; independent raw verifier and production analyser both prove fragmentation");
    return 0;
}

static int create_raw_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   FILE *state)
{
    char detail[512] = {0};
    if (ldtm_raw_fixture(spec, partition, true, detail, sizeof(detail)) != 0 ||
        ldtm_raw_fixture(spec, partition, false, detail, sizeof(detail)) != 0) {
        emit_status(spec->key, "qualification-failed", detail);
        return state_write_status(state, spec, "qualification-failed", detail) == 0 ? 1 : -1;
    }
    const int accepted =
        spec->creator == LDTM_CREATOR_SWAP
            ? production_map_accepts(spec, partition, detail, sizeof(detail))
            : require_fragmentation_state(spec, partition, 1, detail, sizeof(detail));
    if (accepted != 0) {
        emit_status(spec->key, "qualification-failed", detail);
        return state_write_status(state, spec, "qualification-failed", detail) == 0 ? 1 : -1;
    }
    const char *message =
        spec->creator == LDTM_CREATOR_SWAP
            ? "first-party swap header and native page map verified"
            : "first-party raw corpus fully verified; production analyser proved fragmentation";
    emit_status(spec->key, "populated", message);
    return state_write_status(state, spec, "populated", message);
}

static int copy_image_to_partition(const char *image, const char *partition) {
    int input = -1;
    int output = -1;
    unsigned char *buffer = NULL;
    int result = -1;
    input = open(image, O_RDONLY | O_CLOEXEC);
    if (input < 0) goto cleanup;
    output = open(partition, O_WRONLY | O_CLOEXEC);
    if (output < 0) goto cleanup;
    buffer = malloc(1024U * 1024U);
    if (buffer == NULL) goto cleanup;
    for (;;) {
        ssize_t got = read(input, buffer, 1024U * 1024U);
        if (got < 0) {
            if (errno == EINTR) continue;
            goto cleanup;
        }
        if (got == 0) break;
        if (infiltratr_write_full(output, buffer, (size_t)got) != 0) goto cleanup;
    }
    if (fsync(output) != 0) goto cleanup;
    result = 0;
cleanup:
    free(buffer);
    if (output >= 0) (void)close(output);
    if (input >= 0) (void)close(input);
    return result;
}

static int ufs2_summary_ok(const char *path) {
    /* The package-owned host resolves and checks UFS.so; there is no
     * Defragmenter-local UFS parser, formatter or fallback. */
    const char *const argv[] = {"makefs", "--check-ufs2-le", path, NULL};
    return path != NULL && run_process(argv, NULL, 1) == 0;
}

#define LDTM_UFS2_INODE_SIZE 256U
#define LDTM_UFS2_MAGIC_OFFSET 1372U
#define LDTM_UFS2_BSIZE_OFFSET 48U
#define LDTM_UFS2_FSIZE_OFFSET 52U
#define LDTM_UFS2_FRAG_OFFSET 56U
#define LDTM_UFS2_NCG_OFFSET 44U
#define LDTM_UFS2_CBLKNO_OFFSET 12U
#define LDTM_UFS2_IBLKNO_OFFSET 16U
#define LDTM_UFS2_CGSIZE_OFFSET 160U
#define LDTM_UFS2_IPG_OFFSET 184U
#define LDTM_UFS2_FPG_OFFSET 188U
#define LDTM_UFS2_INOPB_OFFSET 120U
#define LDTM_UFS2_CG_MAGIC UINT32_C(0x00090255)
#define LDTM_UFS2_CG_MAGIC_OFFSET 4U
#define LDTM_UFS2_CG_INDEX_OFFSET 12U
#define LDTM_UFS2_CG_IUSEDOFF_OFFSET 92U
#define LDTM_UFS2_IFMT UINT16_C(0170000)
#define LDTM_UFS2_IFREG UINT16_C(0100000)
#define LDTM_UFS2_NDADDR 12U

typedef struct {
    uint64_t superblock_offset;
    uint32_t block_size;
    uint32_t fragment_size;
    uint32_t fragments_per_block;
    uint32_t cylinder_groups;
    uint32_t cylinder_block_fragment;
    uint32_t inode_block_fragment;
    uint32_t cylinder_group_size;
    uint32_t inodes_per_group;
    uint32_t fragments_per_group;
    uint32_t inodes_per_block;
} LdtmUfs2Geometry;

static int ufs2_raw_read(int fd, uint64_t offset,
                         void *buffer, size_t length)
{
    return infiltratr_pread_full(fd, buffer, length, offset);
}

static int ufs2_raw_geometry(int fd, LdtmUfs2Geometry *geometry)
{
    static const uint64_t candidates[] = {
        UINT64_C(65536), UINT64_C(8192),
        UINT64_C(0), UINT64_C(262144)
    };
    uint8_t raw[8192];
    if (geometry == NULL)
        return -1;
    memset(geometry, 0, sizeof(*geometry));
    for (size_t candidate = 0U;
         candidate < sizeof(candidates) / sizeof(candidates[0]);
         ++candidate) {
        if (ufs2_raw_read(fd, candidates[candidate],
                          raw, sizeof(raw)) != 0)
            continue;
        if (raw[LDTM_UFS2_MAGIC_OFFSET] != 0x19U ||
            raw[LDTM_UFS2_MAGIC_OFFSET + 1U] != 0x01U ||
            raw[LDTM_UFS2_MAGIC_OFFSET + 2U] != 0x54U ||
            raw[LDTM_UFS2_MAGIC_OFFSET + 3U] != 0x19U)
            continue;
        geometry->superblock_offset = candidates[candidate];
        geometry->block_size =
            infiltratr_load_le32(raw + LDTM_UFS2_BSIZE_OFFSET);
        geometry->fragment_size =
            infiltratr_load_le32(raw + LDTM_UFS2_FSIZE_OFFSET);
        geometry->fragments_per_block =
            infiltratr_load_le32(raw + LDTM_UFS2_FRAG_OFFSET);
        geometry->cylinder_groups =
            infiltratr_load_le32(raw + LDTM_UFS2_NCG_OFFSET);
        geometry->cylinder_block_fragment =
            infiltratr_load_le32(raw + LDTM_UFS2_CBLKNO_OFFSET);
        geometry->inode_block_fragment =
            infiltratr_load_le32(raw + LDTM_UFS2_IBLKNO_OFFSET);
        geometry->cylinder_group_size =
            infiltratr_load_le32(raw + LDTM_UFS2_CGSIZE_OFFSET);
        geometry->inodes_per_group =
            infiltratr_load_le32(raw + LDTM_UFS2_IPG_OFFSET);
        geometry->fragments_per_group =
            infiltratr_load_le32(raw + LDTM_UFS2_FPG_OFFSET);
        geometry->inodes_per_block =
            infiltratr_load_le32(raw + LDTM_UFS2_INOPB_OFFSET);
        if (geometry->block_size < 4096U ||
            geometry->block_size > 65536U ||
            geometry->fragment_size < 512U ||
            geometry->fragment_size > geometry->block_size ||
            geometry->fragments_per_block == 0U ||
            geometry->fragment_size *
                geometry->fragments_per_block !=
                geometry->block_size ||
            geometry->cylinder_groups == 0U ||
            geometry->cylinder_group_size < 168U ||
            geometry->cylinder_group_size >
                geometry->block_size ||
            geometry->inodes_per_group == 0U ||
            geometry->fragments_per_group == 0U ||
            geometry->inodes_per_block !=
                geometry->block_size / LDTM_UFS2_INODE_SIZE)
            return -1;
        return 0;
    }
    return -1;
}

static int ufs2_raw_pointer(int fd,
                            const LdtmUfs2Geometry *geometry,
                            const uint8_t inode[LDTM_UFS2_INODE_SIZE],
                            uint64_t logical_block,
                            uint64_t *physical_fragment)
{
    const uint64_t nindir = geometry->block_size / 8U;
    uint64_t indices[3] = {0U, 0U, 0U};
    unsigned level = 0U;
    if (logical_block < LDTM_UFS2_NDADDR) {
        *physical_fragment =
            infiltratr_load_le64(
                inode + 112U + logical_block * 8U);
        return 0;
    }
    uint64_t remaining = logical_block - LDTM_UFS2_NDADDR;
    const uint64_t square = nindir * nindir;
    const uint64_t cube = square * nindir;
    if (remaining < nindir) {
        level = 1U;
        indices[0] = remaining;
    } else if ((remaining -= nindir) < square) {
        level = 2U;
        indices[0] = remaining / nindir;
        indices[1] = remaining % nindir;
    } else {
        remaining -= square;
        if (remaining >= cube)
            return -1;
        level = 3U;
        indices[0] = remaining / square;
        remaining %= square;
        indices[1] = remaining / nindir;
        indices[2] = remaining % nindir;
    }

    uint64_t current =
        infiltratr_load_le64(
            inode + 208U + (uint64_t)(level - 1U) * 8U);
    if (current == 0U) {
        *physical_fragment = 0U;
        return 0;
    }
    uint8_t entry[8];
    for (unsigned depth = 0U; depth < level; ++depth) {
        if (current % geometry->fragments_per_block != 0U)
            return -1;
        const uint64_t offset =
            current * geometry->fragment_size +
            indices[depth] * 8U;
        if (ufs2_raw_read(fd, offset, entry, sizeof(entry)) != 0)
            return -1;
        current = infiltratr_load_le64(entry);
        if (current == 0U && depth + 1U != level)
            return -1;
    }
    *physical_fragment = current;
    return 0;
}

static int ufs2_raw_hash_file(
    int fd, const LdtmUfs2Geometry *geometry,
    const uint8_t inode[LDTM_UFS2_INODE_SIZE],
    uint64_t size, char digest_hex[LDTM_HASH_HEX])
{
    EVP_MD_CTX *context = NULL;
    uint8_t *block = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0U;
    uint64_t remaining = size;
    uint64_t logical = 0U;
    int result = -1;

    context = EVP_MD_CTX_new();
    block = malloc(geometry->block_size);
    if (context == NULL || block == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1)
        goto cleanup;

    while (remaining > 0U) {
        uint64_t physical = 0U;
        if (ufs2_raw_pointer(
                fd, geometry, inode, logical,
                &physical) != 0 ||
            physical == 0U)
            goto cleanup;
        const size_t take =
            remaining < geometry->block_size
                ? (size_t)remaining
                : geometry->block_size;
        if (ufs2_raw_read(
                fd, physical * geometry->fragment_size,
                block, take) != 0 ||
            EVP_DigestUpdate(context, block, take) != 1)
            goto cleanup;
        remaining -= take;
        ++logical;
    }
    if (EVP_DigestFinal_ex(
            context, digest, &digest_length) != 1)
        goto cleanup;
    digest_to_hex(digest, digest_length, digest_hex);
    result = 0;

cleanup:
    free(block);
    if (context != NULL)
        EVP_MD_CTX_free(context);
    return result;
}

static int ufs_expected_targets(LdtmTargetRecord targets[LDTM_MAX_TARGET_FILES])
{
    const LdtmFragmentProfile profile = ldtm_fragment_profile(ldtm_find_spec("ufs"));
    const size_t chunk_bytes = (size_t)profile.chunk_kib * 1024U;
    uint8_t *buffer = malloc(chunk_bytes);
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (buffer == NULL || context == NULL) {
        free(buffer);
        EVP_MD_CTX_free(context);
        return -1;
    }
    int result = 0;
    for (unsigned file = 0U; file < LDTM_RAW_FILES && result == 0; ++file) {
        if (EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
            result = -1;
            break;
        }
        if (file < LDTM_TARGET_FILE_COUNT) {
            targets[file].size = ldtm_profile_file_bytes(&profile, file);
            for (uint32_t chunk = 0U; chunk < ldtm_profile_file_chunks(&profile, file); ++chunk) {
                deterministic_fill(buffer, chunk_bytes,
                                   ((uint64_t)file << 48U) ^ ((uint64_t)chunk << 16U) ^
                                       UINT64_C(0x4c44544d));
                if (EVP_DigestUpdate(context, buffer, chunk_bytes) != 1) {
                    result = -1;
                    break;
                }
            }
        } else {
            const unsigned edge = file - LDTM_TARGET_FILE_COUNT;
            const uint32_t bytes = ldtm_edge_case_sizes[edge];
            targets[file].size = bytes;
            deterministic_fill(buffer, bytes, edge_case_seed(edge, bytes));
            if (EVP_DigestUpdate(context, buffer, bytes) != 1) {
                result = -1;
                break;
            }
        }
        uint8_t digest[EVP_MAX_MD_SIZE];
        unsigned length = 0U;
        if (EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32U) {
            result = -1;
            break;
        }
        digest_to_hex(digest, length, targets[file].sha256);
    }
    EVP_MD_CTX_free(context);
    free(buffer);
    return result;
}

static int verify_ufs2_raw_payload(const char *path, char *detail, size_t detail_capacity)
{
    LdtmUfs2Geometry geometry;
    uint8_t *cg = NULL;
    uint8_t inode[LDTM_UFS2_INODE_SIZE];
    uint8_t matched[LDTM_MAX_TARGET_FILES] = {0};
    uint64_t regular_files = 0U;
    LdtmTargetRecord targets[LDTM_MAX_TARGET_FILES];
    const size_t target_count = LDTM_RAW_FILES;
    int fd = -1;
    int result = -1;

    if (detail != NULL && detail_capacity > 0U)
        detail[0] = '\0';
    if (path == NULL || ufs_expected_targets(targets) != 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ufs2_raw_geometry(fd, &geometry) != 0)
        goto cleanup;
    cg = malloc(geometry.cylinder_group_size);
    if (cg == NULL)
        goto cleanup;

    for (uint32_t group = 0U;
         group < geometry.cylinder_groups; ++group) {
        const uint64_t group_base =
            (uint64_t)group * geometry.fragments_per_group;
        const uint64_t cg_offset =
            (group_base +
             geometry.cylinder_block_fragment) *
            geometry.fragment_size;
        if (ufs2_raw_read(
                fd, cg_offset, cg,
                geometry.cylinder_group_size) != 0 ||
            infiltratr_load_le32(
                cg + LDTM_UFS2_CG_MAGIC_OFFSET) !=
                LDTM_UFS2_CG_MAGIC ||
            infiltratr_load_le32(
                cg + LDTM_UFS2_CG_INDEX_OFFSET) != group)
            goto cleanup;
        const uint32_t iused =
            infiltratr_load_le32(
                cg + LDTM_UFS2_CG_IUSEDOFF_OFFSET);
        const uint64_t map_bytes =
            ((uint64_t)geometry.inodes_per_group + 7U) / 8U;
        if (iused < 168U ||
            (uint64_t)iused + map_bytes >
                geometry.cylinder_group_size)
            goto cleanup;

        for (uint32_t local = 0U;
             local < geometry.inodes_per_group; ++local) {
            if ((cg[iused + (local >> 3U)] &
                 (uint8_t)(1U << (local & 7U))) == 0U)
                continue;
            const uint32_t inode_block =
                local / geometry.inodes_per_block;
            const uint32_t inode_slot =
                local % geometry.inodes_per_block;
            const uint64_t inode_fragment =
                group_base +
                geometry.inode_block_fragment +
                (uint64_t)inode_block *
                    geometry.fragments_per_block;
            const uint64_t inode_offset =
                inode_fragment * geometry.fragment_size +
                (uint64_t)inode_slot *
                    LDTM_UFS2_INODE_SIZE;
            if (ufs2_raw_read(
                    fd, inode_offset, inode,
                    sizeof(inode)) != 0)
                goto cleanup;
            const uint16_t mode =
                infiltratr_load_le16(inode);
            if ((mode & LDTM_UFS2_IFMT) !=
                LDTM_UFS2_IFREG)
                continue;
            const uint64_t size =
                infiltratr_load_le64(inode + 16U);
            ++regular_files;

            size_t candidate_count = 0U;
            for (size_t target = 0U;
                 target < target_count; ++target) {
                if (!matched[target] &&
                    targets[target].size == size)
                    candidate_count++;
            }
            if (candidate_count == 0U)
                continue;

            char hash[LDTM_HASH_HEX];
            if (ufs2_raw_hash_file(
                    fd, &geometry, inode, size, hash) != 0)
                goto cleanup;
            int found = 0;
            for (size_t target = 0U;
                 target < target_count; ++target) {
                if (!matched[target] &&
                    targets[target].size == size &&
                    strcmp(targets[target].sha256, hash) == 0) {
                    matched[target] = 1U;
                    found = 1;
                    break;
                }
            }
            if (!found)
                goto cleanup;
        }
    }

    for (size_t target = 0U;
         target < target_count; ++target) {
        if (!matched[target])
            goto cleanup;
    }
    if (regular_files != (uint64_t)target_count)
        goto cleanup;

    if (detail != NULL && detail_capacity > 0U)
        (void)snprintf(detail, detail_capacity,
                       "independent raw UFS2 verifier matched all %zu canonical payloads including "
                       "boundary-sized files",
                       target_count);
    result = 0;

cleanup:
    free(cg);
    if (fd >= 0)
        (void)close(fd);
    if (result != 0 && detail != NULL &&
        detail_capacity > 0U && detail[0] == '\0')
        (void)snprintf(
            detail, detail_capacity,
            "independent raw UFS2 payload verification failed");
    return result;
}

static int create_ufs_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   const char *work, FILE *state) {
    char source[PATH_MAX];
    char image[PATH_MAX];
    const char *const wipe_argv[] = {"wipefs", "--all", "--force", partition, NULL};
    const char *makefs_argv[12];
    char image_size[32];
    if (!ldtm_program_available("makefs")) {
        emit_status(spec->key, "skipped", "bundled first-party UFS2 creator is missing");
        (void)state_write_status(state, spec, "skipped",
                                 "bundled first-party UFS2 creator is missing");
        return 1;
    }
    if (run_process(wipe_argv, NULL, 0) != 0 ||
        snprintf(source, sizeof(source), "%s/ufs-source", work) <= 0 ||
        snprintf(image, sizeof(image), "%s/ufs.img", work) <= 0 ||
        ensure_directory(source, 0755) != 0) {
        emit_status(spec->key, "format-failed", "could not prepare UFS image workspace");
        (void)state_write_status(state, spec, "format-failed", "UFS workspace preparation failed");
        return 1;
    }
    if (generate_ufs_source(source) != 0) {
        emit_status(spec->key, "format-failed", "could not build deterministic UFS source tree");
        (void)state_write_status(state, spec, "format-failed", "UFS source tree generation failed");
        return 1;
    }
    makefs_argv[0] = "makefs";
    makefs_argv[1] = "-t";
    makefs_argv[2] = "ffs";
    makefs_argv[3] = "-B";
    makefs_argv[4] = "little";
    makefs_argv[5] = "-s";
    /* Preserve the qualified 2047 MiB UFS2 geometry within the 2 GiB slot. */
    const uint32_t ufs_image_mib =
        spec->size_mib >= 2048U ? 2047U : spec->size_mib;
    if (snprintf(image_size, sizeof(image_size), "%um",
                 ufs_image_mib) <= 0)
        return -1;
    makefs_argv[6] = image_size;
    makefs_argv[7] = "-o";
    /* This argv contract resolves exclusively to the package-owned C helper. */
    makefs_argv[8] =
        "version=2,bsize=8192,fsize=1024,minfree=5";
    makefs_argv[9] = image;
    makefs_argv[10] = source;
    makefs_argv[11] = NULL;
    const int makefs_result = run_process(makefs_argv, NULL, 0);
    if (makefs_result != 0) {
        char detail[192];
        (void)snprintf(
            detail, sizeof(detail),
            "makefs failed while building the bounded %u MiB UFS2 image",
            ufs_image_mib);
        emit_status(spec->key, "format-failed", detail);
        (void)state_write_status(state, spec, "format-failed", detail);
        return 1;
    }
    if (!ufs2_summary_ok(image)) {
        emit_status(spec->key, "qualification-failed",
                    "makefs completed but UFS.so rejected its UFS2 image");
        (void)state_write_status(
            state, spec, "qualification-failed",
            "makefs output failed UFS.so UFS2 geometry validation");
        return 1;
    }
    printf("+ copy verified UFS2 image %s -> %s\n", image, partition);
    fflush(stdout);
    if (copy_image_to_partition(image, partition) != 0) {
        emit_status(
            spec->key, "format-failed",
            "UFS2 image copy failed");
        (void)state_write_status(
            state, spec, "format-failed",
            "UFS2 image copy failed");
        return 1;
    }
    if (!ufs2_summary_ok(partition)) {
        emit_status(spec->key, "qualification-failed",
                    "copied UFS2 image failed UFS.so geometry validation");
        (void)state_write_status(state, spec, "qualification-failed",
                                 "UFS2 partition validation failed");
        return 1;
    }
    {
        char detail[512] = {0};
        if (verify_ufs2_raw_payload(partition, detail, sizeof(detail)) != 0) {
            emit_status(
                spec->key, "qualification-failed", detail);
            (void)state_write_status(
                state, spec, "qualification-failed", detail);
            return 1;
        }
    }
    {
        char detail[512];
        if (require_fragmentation_state(
                spec, partition, 1, detail, sizeof(detail)) != 0) {
            emit_status(spec->key, "qualification-failed", detail);
            (void)state_write_status(
                state, spec, "qualification-failed", detail);
            return 1;
        }
    }
    if (state_write_status(state, spec, "populated",
                           "first-party bounded UFS2 payload created and completely verified; "
                           "production analyser proved fragmentation") != 0)
        return -1;
    emit_status(spec->key, "populated",
                "first-party bounded UFS2 payload created and completely verified; production "
                "analyser proved fragmentation");
    return 0;
}

static int create_apfs_and_populate(const LdtmFilesystemSpec *spec,
                                    const char *partition,
                                    FILE *state)
{
    const char *const wipe_argv[] = {
        "wipefs", "--all", "--force", partition, NULL
    };
    char detail[512] = {0};
    if (run_process(wipe_argv, NULL, 0) != 0) {
        emit_status(spec->key, "format-failed",
                    "could not clear old filesystem signatures");
        (void)state_write_status(
            state, spec, "format-failed", "wipefs failed");
        return 1;
    }
    printf("+ built-in raw C APFS fixture creator %s\n", partition);
    fflush(stdout);
    if (ldtm_format_apfs_volume(partition) != 0) {
        emit_status(spec->key, "format-failed",
                    "built-in C APFS creator failed");
        (void)state_write_status(
            state, spec, "format-failed",
            "built-in C APFS creator failed");
        return 1;
    }
    if (ldtm_verify_apfs_payload(
            partition, detail, sizeof(detail)) != 0) {
        emit_status(
            spec->key, "formatted-unpopulated",
            detail[0] != '\0'
                ? detail : "APFS fixture self-check failed");
        (void)state_write_status(
            state, spec, "formatted-unpopulated",
            "APFS fixture self-check failed");
        return 1;
    }
    {
        char analyser_detail[512];
        if (require_fragmentation_state(
                spec, partition, 1,
                analyser_detail, sizeof(analyser_detail)) != 0) {
            emit_status(spec->key, "qualification-failed",
                        analyser_detail);
            (void)state_write_status(
                state, spec, "qualification-failed",
                analyser_detail);
            return 1;
        }
    }
    if (state_write_status(
            state, spec, "populated",
            "first-party raw C capped APFS fixture created; independent payload verifier and production analyser prove fragmentation") != 0)
        return -1;
    emit_status(
        spec->key, "populated",
        "first-party raw C capped APFS fixture created; independent payload verifier and production analyser prove fragmentation");
    return 0;
}

static int recursive_remove(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return rmdir(path);
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat st;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!infiltratr_path_join(child, sizeof(child), path, entry->d_name)) {
            (void)closedir(directory);
            return -1;
        }
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) (void)recursive_remove(child); else (void)unlink(child);
    }
    (void)closedir(directory);
    return rmdir(path);
}

int ldtm_worker_prepare(const char *device, const char *confirmed_device,
                        const char *confirmed_fingerprint) {
    char canonical[PATH_MAX];
    char confirmed[PATH_MAX];
    char current_fingerprint[LDTM_HASH_HEX];
    char safety[512];
    char script[8192];
    char work_template[] = "/tmp/defragmenter-test-media.XXXXXX";
    char *work;
    char state_path[PATH_MAX];
    LdtmPartitionMap map;
    FILE *state = NULL;
    size_t index;
    int result = 1;
    if (geteuid() != 0) {
        fputs("Test-media prepare worker must run as root.\n", stderr);
        return 2;
    }
    if (!ldtm_required_modules_available(safety, sizeof(safety))) {
        fprintf(stderr, "%s\n", safety);
        return 2;
    }
    if (ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0 ||
        ldtm_canonicalize_device(confirmed_device, confirmed, sizeof(confirmed)) != 0 ||
        strcmp(canonical, confirmed) != 0) {
        fputs("Destructive confirmation does not match the selected device.\n", stderr);
        return 2;
    }
    if (confirmed_fingerprint == NULL ||
        strlen(confirmed_fingerprint) != LDTM_HASH_HEX - 1U ||
        ldtm_device_fingerprint(canonical, current_fingerprint) != 0 ||
        strcmp(current_fingerprint, confirmed_fingerprint) != 0) {
        fputs("The selected physical disk changed after confirmation; refusing destructive preparation.\n",
              stderr);
        return 2;
    }
    if (ldtm_device_safety_check(canonical, 0, safety, sizeof(safety)) != 0) {
        fprintf(stderr, "%s\n", safety);
        return 2;
    }
    printf("Safety check: %s\n", safety);
    fflush(stdout);
    if (unmount_descendants(canonical) != 0) {
        fputs("Unable to unmount all descendants.\n", stderr);
        return 2;
    }
    if (ldtm_device_fingerprint(canonical, current_fingerprint) != 0 ||
        strcmp(current_fingerprint, confirmed_fingerprint) != 0) {
        fputs("The physical disk identity changed before repartitioning; refusing to continue.\n",
              stderr);
        return 2;
    }
    /*
     * The earlier system-storage test deliberately permits ordinary desktop
     * /media mounts so this worker can unmount them itself.  Recheck the full
     * related-device mount/swap topology after that unmount and immediately
     * before the destructive partition-table write.
     */
    if (ld_path_is_mounted(canonical)) {
        fputs("The selected disk or related storage is still mounted or active; refusing destructive repartitioning.\n",
              stderr);
        return 2;
    }
    if (ldtm_build_sfdisk_script(script, sizeof(script)) != 0) return 2;
    {
        const char *const argv[] = {"sfdisk", "--wipe", "always", "--lock", canonical, NULL};
        if (run_process(argv, script, 0) != 0) return 2;
    }
    if (settle_partitions(canonical, &map) != 0) {
        fputs("New partition table did not settle into the complete labelled layout.\n",
              stderr);
        return 2;
    }
    work = mkdtemp(work_template);
    if (work == NULL) return 2;
    if (state_path_for_device(canonical, state_path, sizeof(state_path)) != 0)
        goto cleanup;
    state = open_state_stream(state_path, 1);
    if (state == NULL) {
        fprintf(stderr, "Unable to create protected Test Media state: %s\n",
                strerror(errno));
        goto cleanup;
    }
    if (fprintf(state, "schema\t3\ncorpus\t1\ndevice\t%s\nfingerprint\t%s\n", canonical,
                confirmed_fingerprint) < 0 ||
        state_flush(state) != 0)
        goto cleanup;

    {
        int failures = 0;
        for (index = 0U; index < ldtm_spec_count(); ++index) {
            const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
            const char *partition = partition_for_label(&map, spec->label);
            int filesystem_result = 0;
            printf("\n=== %s: %s ===\n", spec->key,
                   partition != NULL ? partition : "missing partition");
            fflush(stdout);
            if (partition == NULL) {
                emit_status(
                    spec->key, "failed",
                    "partition label was not found after repartitioning");
                (void)state_write_status(
                    state, spec, "failed", "partition label missing");
                failures++;
                continue;
            }
            if (spec->creator == LDTM_CREATOR_MANUAL) {
                emit_status(spec->key, "qualification-failed", spec->note);
                (void)state_write_status(
                    state, spec, "qualification-failed", spec->note);
                failures++;
                continue;
            }
            if (spec->creator == LDTM_CREATOR_AFFS ||
                spec->creator == LDTM_CREATOR_PFS3) {
                filesystem_result =
                    create_amiga_and_populate(spec, partition, state);
            } else if (spec->creator == LDTM_CREATOR_APFS) {
                filesystem_result =
                    create_apfs_and_populate(spec, partition, state);
            } else if (spec->creator == LDTM_CREATOR_UFS) {
                filesystem_result =
                    create_ufs_and_populate(
                        spec, partition, work, state);
            } else {
                filesystem_result = create_raw_and_populate(spec, partition, state);
            }
            if (filesystem_result < 0)
                goto cleanup;
            if (filesystem_result > 0)
                failures++;
        }
        result = failures == 0 ? 0 : 1;
        printf(
            "\nTest disk preparation finished with %d qualification failure%s. State: %s\n",
            failures, failures == 1 ? "" : "s", state_path);
        fflush(stdout);
    }

cleanup:
    if (state != NULL) (void)fclose(state);
    (void)recursive_remove(work);
    return result;
}

static void append_bounded_text(char *destination, size_t capacity,
                                const char *source)
{
    if (destination == NULL || capacity == 0U || source == NULL)
        return;
    size_t used = strnlen(destination, capacity);
    if (used >= capacity - 1U)
        return;
    const size_t available = capacity - used - 1U;
    const size_t amount = strnlen(source, available);
    if (amount != 0U)
        memcpy(destination + used, source, amount);
    destination[used + amount] = '\0';
}

static void describe_unpopulated_filesystem(
    const LdtmVerifyFilesystem *expected,
    char *detail, size_t detail_capacity)
{
    if (detail == NULL || detail_capacity == 0U)
        return;
    detail[0] = '\0';
    append_bounded_text(
        detail, detail_capacity,
        "Build Test Disk did not produce a populated filesystem");
    if (expected != NULL && expected->build_status[0] != '\0') {
        append_bounded_text(detail, detail_capacity, " (");
        append_bounded_text(
            detail, detail_capacity, expected->build_status);
        append_bounded_text(detail, detail_capacity, ")");
    }
    if (expected != NULL && expected->build_detail[0] != '\0') {
        append_bounded_text(detail, detail_capacity, ": ");
        append_bounded_text(
            detail, detail_capacity, expected->build_detail);
    }
}

static bool state_hash(const char *text)
{
    if (strlen(text) != 64U)
        return false;
    for (size_t n = 0U; n < 64U; ++n)
        if (!isxdigit((unsigned char)text[n]))
            return false;
    return true;
}

/* Saved state binds this build to the physical disk and reports build results.
 * It cannot enumerate, resize, or remove expected payloads. Corpus version 1
 * is compiled into the native verifiers for every slot. Legacy schema 2 media
 * must be rebuilt; accepting it would reintroduce the mutable-list authority. */
static int parse_verify_state(FILE *stream, const char *device,
                              LdtmVerifyFilesystem state[LDTM_SPEC_COUNT],
                              char saved_fingerprint[LDTM_HASH_HEX])
{
    char line[LDTM_LINE_MAX];
    unsigned headers = 0U;
    uint32_t filesystems = 0U;
    memset(state, 0, sizeof(*state) * LDTM_SPEC_COUNT);
    saved_fingerprint[0] = '\0';
    while (fgets(line, sizeof(line), stream) != NULL) {
        const size_t length = strlen(line);
        if (length == 0U || line[length - 1U] != '\n')
            return -1;
        line[length - 1U] = '\0';
        char *fields[5], *cursor = line;
        size_t count = 0U;
        do {
            if (count == 5U)
                return -1;
            fields[count++] = cursor;
            char *tab = strchr(cursor, '\t');
            if (tab == NULL)
                break;
            *tab = '\0';
            cursor = tab + 1U;
        } while (true);
        if (count == 2U && strcmp(fields[0], "schema") == 0 && strcmp(fields[1], "3") == 0) {
            if ((headers & 1U) != 0U)
                return -1;
            headers |= 1U;
        } else if (count == 2U && strcmp(fields[0], "corpus") == 0 && strcmp(fields[1], "1") == 0) {
            if ((headers & 2U) != 0U)
                return -1;
            headers |= 2U;
        } else if (count == 2U && strcmp(fields[0], "device") == 0 &&
                   strcmp(fields[1], device) == 0) {
            if ((headers & 4U) != 0U)
                return -1;
            headers |= 4U;
        } else if (count == 2U && strcmp(fields[0], "fingerprint") == 0 && state_hash(fields[1])) {
            if ((headers & 8U) != 0U)
                return -1;
            headers |= 8U;
            memcpy(saved_fingerprint, fields[1], 65U);
        } else if (count == 4U && strcmp(fields[0], "fs") == 0) {
            const LdtmFilesystemSpec *spec = ldtm_find_spec(fields[1]);
            if (spec == NULL)
                return -1;
            const size_t index = (size_t)(spec - ldtm_specs());
            const uint32_t bit = UINT32_C(1) << index;
            if ((filesystems & bit) != 0U ||
                strlen(fields[2]) >= sizeof(state[index].build_status) ||
                strlen(fields[3]) >= sizeof(state[index].build_detail))
                return -1;
            if (strcmp(fields[2], "populated") != 0 && strcmp(fields[2], "failed") != 0 &&
                strcmp(fields[2], "format-failed") != 0 &&
                strcmp(fields[2], "formatted-unpopulated") != 0 &&
                strcmp(fields[2], "qualification-failed") != 0 && strcmp(fields[2], "skipped") != 0)
                return -1;
            filesystems |= bit;
            state[index].populated = strcmp(fields[2], "populated") == 0;
            memcpy(state[index].build_status, fields[2], strlen(fields[2]) + 1U);
            memcpy(state[index].build_detail, fields[3], strlen(fields[3]) + 1U);
        } else
            return -1;
    }
    return !ferror(stream) && headers == 15U && filesystems == (UINT32_C(1) << LDTM_SPEC_COUNT) - 1U
               ? 0
               : -1;
}

static int load_verify_state(const char *state_path, const char *device,
                             LdtmVerifyFilesystem state[LDTM_SPEC_COUNT])
{
    FILE *stream = open_state_stream(state_path, 0);
    if (stream == NULL)
        return -1;
    char saved[LDTM_HASH_HEX], current[LDTM_HASH_HEX];
    int result = parse_verify_state(stream, device, state, saved);
    if (fclose(stream) != 0)
        result = -1;
    if (result != 0 || ldtm_device_fingerprint(device, current) != 0 || strcmp(saved, current) != 0)
        return -1;
    return 0;
}

int ldtm_worker_verify(const char *device) {
    char canonical[PATH_MAX];
    char safety[512];
    char state_path[PATH_MAX];
    LdtmPartitionMap map;
    LdtmVerifyFilesystem expected[LDTM_SPEC_COUNT];
    size_t index;
    int failures = 0;
    if (geteuid() != 0) {
        fputs("Test-media verify worker must run as root.\n", stderr);
        return 2;
    }
    if (ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0 ||
        ldtm_device_safety_check(canonical, 0, safety, sizeof(safety)) != 0) {
        fprintf(stderr, "%s\n", safety);
        return 2;
    }
    if (state_path_for_device(canonical, state_path, sizeof(state_path)) != 0 ||
        load_verify_state(state_path, canonical, expected) != 0) {
        fprintf(stderr, "No matching protected test-media state found for %s.\n", canonical);
        return 2;
    }
    if (unmount_descendants(canonical) != 0 ||
        load_partition_map(canonical, &map) != 0)
        return 2;
    for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        const char *partition;
        if (!expected[index].populated) {
            char detail[768];
            describe_unpopulated_filesystem(
                &expected[index], detail, sizeof(detail));
            emit_status(spec->key, "verify-failed", detail);
            failures++;
            continue;
        }
        printf("\n=== verify %s ===\n", spec->key);
        fflush(stdout);

        partition = partition_for_label(&map, spec->label);
        if (partition == NULL) {
            emit_status(spec->key, "verify-failed",
                        "partition label is missing");
            failures++;
            continue;
        }

        if (spec->creator == LDTM_CREATOR_APFS) {
            char detail[512] = {0};
            if (ldtm_verify_apfs_payload_after_defrag(
                    partition, detail, sizeof(detail)) != 0 ||
                require_fragmentation_state(
                    spec, partition, 0,
                    detail, sizeof(detail)) != 0) {
                emit_status(
                    spec->key, "verify-failed",
                    detail[0] != '\0'
                        ? detail
                        : "raw C APFS post-defrag verification failed");
                failures++;
            } else {
                emit_status(
                    spec->key, "verified",
                    "APFS payload is byte-identical and the production analyser reports zero fragmentation");
            }
            continue;
        }

        if (spec->creator == LDTM_CREATOR_AFFS ||
            spec->creator == LDTM_CREATOR_PFS3) {
            const uint8_t dostype =
                strcmp(spec->key, "ofs") == 0 ? (uint8_t)0 : (uint8_t)1;
            const LdtmFragmentProfile profile =
                ldtm_fragment_profile(spec);
            char detail[512] = {0};
            if (ldtm_verify_amiga_payload_after_defrag(
                    partition, dostype, &profile,
                    detail, sizeof(detail)) != 0 ||
                require_fragmentation_state(
                    spec, partition, 0,
                    detail, sizeof(detail)) != 0) {
                emit_status(
                    spec->key, "verify-failed",
                    detail[0] != '\0'
                        ? detail
                        : "raw C Amiga post-defrag verification failed");
                failures++;
            } else {
                emit_status(
                    spec->key, "verified",
                    "Amiga payload is byte-identical and the production analyser reports zero file/directory fragmentation");
            }
            continue;
        }

        if (spec->creator == LDTM_CREATOR_SWAP) {
            char detail[512] = {0};
            if (ldtm_raw_fixture(spec, partition, false, detail, sizeof(detail)) != 0 ||
                production_map_accepts(spec, partition, detail, sizeof(detail)) != 0) {
                emit_status(spec->key, "verify-failed", detail);
                failures++;
            } else {
                emit_status(
                    spec->key, "verified",
                    "swap header, reserved/bad-page geometry and inactive page map are accepted by the production analyser");
            }
            continue;
        }

        if (spec->creator == LDTM_CREATOR_UFS) {
            char detail[512] = {0};
            if (require_fragmentation_state(spec, partition, 0, detail, sizeof(detail)) != 0 ||
                verify_ufs2_raw_payload(partition, detail, sizeof(detail)) != 0) {
                emit_status(spec->key, "verify-failed", detail);
                failures++;
            } else {
                emit_status(
                    spec->key, "verified",
                    "UFS2 production analyser reports zero fragmentation and the independent raw verifier matched all retained payload bytes");
            }
            continue;
        }

        {
            char detail[512];
            if (require_fragmentation_state(spec, partition,
                                            spec->creator == LDTM_CREATOR_ZFS ? 1 : 0, detail,
                                            sizeof(detail)) != 0) {
                emit_status(spec->key, "verify-failed", detail);
                failures++;
                continue;
            }
        }

        {
            char detail[512] = {0};
            if (ldtm_raw_fixture(spec, partition, false, detail, sizeof(detail)) != 0) {
                emit_status(spec->key, "verify-failed", detail);
                failures++;
            } else
                emit_status(spec->key, "verified",
                            "every canonical file and boundary-sized payload byte matched the "
                            "native corpus");
        }
    }
    if (failures != 0) {
        fprintf(stderr,
                "Test-media verification failed closed: %d filesystem qualification failure%s.\n",
                failures, failures == 1 ? "" : "s");
        return 1;
    }
    return 0;
}


static int run_production_defrag(
    const char *engine, const LdtmFilesystemSpec *spec,
    const char *partition, const char *operation, const char *journal)
{
    const bool growth = strcmp(operation, "growth-defrag") == 0;
    const char *argv[16];
    size_t arg = 0U;
    argv[arg++] = engine;
    argv[arg++] = operation;
    argv[arg++] = partition;
    argv[arg++] = "--filesystem";
    argv[arg++] = spec->key;
    argv[arg++] = "--write";
    argv[arg++] = "--confirm";
    argv[arg++] = partition;
    argv[arg++] = "--journal";
    argv[arg++] = journal;
    if (growth) {
        argv[arg++] = "--growth-percent";
        argv[arg++] = "10";
    }
    argv[arg] = NULL;
    return run_process(argv, NULL, 0);
}

int ldtm_worker_qualify(const char *device)
{
    char canonical[PATH_MAX];
    char safety[512];
    char state_path[PATH_MAX];
    char fingerprint[LDTM_HASH_HEX];
    char engine[PATH_MAX];
    LdtmPartitionMap map;
    LdtmVerifyFilesystem expected[LDTM_SPEC_COUNT];
    int failures = 0;

    if (geteuid() != 0) {
        fputs("Test-media qualification worker must run as root.\n", stderr);
        return 2;
    }
    if (ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0 ||
        ldtm_device_safety_check(canonical, 0, safety, sizeof(safety)) != 0) {
        fprintf(stderr, "%s\n", safety);
        return 2;
    }
    if (state_path_for_device(canonical, state_path, sizeof(state_path)) != 0 ||
        load_verify_state(state_path, canonical, expected) != 0) {
        fprintf(stderr,
                "No matching protected test-media state found for %s.\n",
                canonical);
        return 2;
    }
    if (ldtm_device_fingerprint(canonical, fingerprint) != 0) {
        fputs("Unable to rebind the protected test disk identity.\n", stderr);
        return 2;
    }
    if (ldtm_resolve_program(
            "/System/Commands/Defragmenter/defragmenter-operation-engine",
            engine, sizeof(engine)) != 0) {
        fputs("The installed production Defragmenter operation engine is unavailable.\n",
              stderr);
        return 2;
    }
    if (unmount_descendants(canonical) != 0 ||
        load_partition_map(canonical, &map) != 0) {
        fputs("Unable to obtain an unmounted qualified partition map.\n", stderr);
        return 2;
    }

    for (size_t index = 0U; index < LDTM_SPEC_COUNT; ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        const char *partition;
        char journal[PATH_MAX];
        char detail[512] = {0};

        if (!expected[index].populated) {
            describe_unpopulated_filesystem(
                &expected[index], detail, sizeof(detail));
            emit_status(spec->key, "qualification-failed", detail);
            failures++;
            continue;
        }
        partition = partition_for_label(&map, spec->label);
        if (partition == NULL) {
            emit_status(spec->key, "qualification-failed",
                        "partition label is missing");
            failures++;
            continue;
        }

        if (spec->creator == LDTM_CREATOR_ZFS ||
            spec->creator == LDTM_CREATOR_SWAP) {
            emit_status(
                spec->key, "qualification-skipped",
                spec->creator == LDTM_CREATOR_ZFS
                    ? "ZFS is analysis-only; retained payload and exact analysis will still be verified"
                    : "swap has no Defragment operation; its native map will still be verified");
            continue;
        }

        if (snprintf(
                journal, sizeof(journal),
                LDTM_STATE_ROOT "/qualification-%.16s-%s.journal",
                fingerprint, spec->key) <= 0) {
            emit_status(spec->key, "qualification-failed",
                        "could not construct the protected transaction journal path");
            failures++;
            continue;
        }

        printf("\n=== qualify %s on %s ===\n", spec->key, partition);
        fflush(stdout);

        /*
         * A previous interrupted qualification deliberately leaves the real
         * production journal behind. Recover it with the production engine
         * before starting a new destructive transaction.
         */
        if (access(journal, F_OK) == 0) {
            printf("+ recovering interrupted production transaction\n");
            fflush(stdout);
            if (run_production_defrag(
                    engine, spec, partition, "recover", journal) != 0) {
                emit_status(
                    spec->key, "qualification-failed",
                    "production Recover could not complete the previous test transaction");
                failures++;
                continue;
            }
        }

        printf("+ production Defragment\n");
        fflush(stdout);
        if (run_production_defrag(
                engine, spec, partition, "defrag", journal) != 0 ||
            require_fragmentation_state(
                spec, partition, 0, detail, sizeof(detail)) != 0) {
            emit_status(
                spec->key, "qualification-failed",
                detail[0] != '\0'
                    ? detail
                    : "production Defragment failed");
            failures++;
            continue;
        }

        /*
         * Growth Defrag is a separate writer contract. Run it after packed
         * Defragment so the final test medium proves both mutation paths on
         * the same retained payload before independent byte verification.
         */
        detail[0] = '\0';
        printf("+ production Growth Defrag (10%% reserve)\n");
        fflush(stdout);
        if (run_production_defrag(
                engine, spec, partition, "growth-defrag", journal) != 0 ||
            require_fragmentation_state(
                spec, partition, 0, detail, sizeof(detail)) != 0) {
            emit_status(
                spec->key, "qualification-failed",
                detail[0] != '\0'
                    ? detail
                    : "production Growth Defrag failed");
            failures++;
            continue;
        }

        emit_status(
            spec->key, "qualified",
            "production Defragment and Growth Defrag completed; exact analyser reports zero fragmentation");
    }

    if (failures != 0) {
        fprintf(stderr,
                "Production mutation qualification failed closed for %d filesystem%s; payload verification was not used to hide those failures.\n",
                failures, failures == 1 ? "" : "s");
        return 1;
    }

    puts("\nAll writable production engines completed. Verifying retained payload bytes independently...");
    fflush(stdout);
    return ldtm_worker_verify(canonical);
}
