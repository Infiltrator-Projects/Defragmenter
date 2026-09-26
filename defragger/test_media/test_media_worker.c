// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "ufs_native.h"
#include "zfs_native.h"
#include "ld_device.h"

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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LDTM_MAX_TARGET_FILES 8U
#define LDTM_HASH_HEX 65U
#define LDTM_LINE_MAX 4096U
#define LDTM_CAPTURE_MAX (8U * 1024U * 1024U)

static const uint32_t ldtm_edge_case_sizes[] = {
    0U, 1U, 511U, 512U, 513U, 4095U, 4096U, 4097U
};
#define LDTM_EDGE_CASE_COUNT \
    (sizeof(ldtm_edge_case_sizes) / sizeof(ldtm_edge_case_sizes[0]))

typedef struct {
    char relative_path[192];
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
    char pool[96];
    LdtmTargetRecord targets[LDTM_MAX_TARGET_FILES];
    size_t target_count;
    uint32_t directory_entries;
} LdtmVerifyFilesystem;

static void digest_to_hex(const unsigned char *digest, unsigned int length,
                          char output[LDTM_HASH_HEX]);

static void emit_status(const char *filesystem, const char *status, const char *detail) {
    const char *safe_detail = detail != NULL ? detail : "";
    printf("LDTM_STATUS\t%s\t%s\t%s\n", filesystem, status, safe_detail);
    fflush(stdout);
}

static int run_process(const char *const argv[], const char *stdin_text, int quiet) {
    int input_pipe[2] = {-1, -1};
    pid_t child;
    int status = 0;
    char program[PATH_MAX];
    if (argv == NULL || argv[0] == NULL ||
        ldtm_resolve_program(argv[0], program, sizeof(program)) != 0)
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
    if (stdin_text != NULL && pipe(input_pipe) != 0) return -1;
    child = fork();
    if (child < 0) {
        if (input_pipe[0] >= 0) {
            close(input_pipe[0]);
            close(input_pipe[1]);
        }
        return -1;
    }
    if (child == 0) {
        if (stdin_text != NULL) {
            (void)close(input_pipe[1]);
            if (dup2(input_pipe[0], STDIN_FILENO) < 0) _exit(126);
            (void)close(input_pipe[0]);
        }
        execv(program, (char *const *)argv);
        _exit(127);
    }
    if (stdin_text != NULL) {
        const size_t length = strlen(stdin_text);
        (void)close(input_pipe[0]);
        if (infiltratr_write_full(input_pipe[1], stdin_text, length) != 0) {
            (void)close(input_pipe[1]);
            (void)waitpid(child, &status, 0);
            return -1;
        }
        (void)close(input_pipe[1]);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int capture_process(const char *const argv[], char **output) {
    int output_pipe[2];
    pid_t child;
    int status = 0;
    size_t capacity = 0U;
    size_t used = 0U;
    char *buffer = NULL;
    char program[PATH_MAX];

    if (output == NULL || argv == NULL || argv[0] == NULL ||
        ldtm_resolve_program(argv[0], program, sizeof(program)) != 0)
        return -1;
    *output = NULL;
    if (pipe(output_pipe) != 0) return -1;

    child = fork();
    if (child < 0) {
        (void)close(output_pipe[0]);
        (void)close(output_pipe[1]);
        return -1;
    }
    if (child == 0) {
        (void)close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        (void)close(output_pipe[1]);
        execv(program, (char *const *)argv);
        _exit(127);
    }

    (void)close(output_pipe[1]);
    if (!infiltratr_array_reserve((void **)&buffer, &capacity, 1U,
                                  4096U, 4096U)) {
        (void)close(output_pipe[0]);
        (void)kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        return -1;
    }

    for (;;) {
        if (used == LDTM_CAPTURE_MAX) {
            unsigned char extra = 0U;
            ssize_t probe;
            do {
                probe = read(output_pipe[0], &extra, 1U);
            } while (probe < 0 && errno == EINTR);
            if (probe == 0) break;
            free(buffer);
            (void)close(output_pipe[0]);
            (void)kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            errno = probe < 0 ? errno : EOVERFLOW;
            return -1;
        }

        const size_t remaining_limit = LDTM_CAPTURE_MAX - used;
        if (capacity - used < 2048U) {
            size_t required = 0U;
            size_t wanted = remaining_limit < 2048U
                ? remaining_limit + 1U : 2049U;
            if (!infiltratr_size_add_checked(used, wanted, &required) ||
                required > LDTM_CAPTURE_MAX + 1U ||
                !infiltratr_array_reserve((void **)&buffer, &capacity, 1U,
                                          required, 4096U)) {
                free(buffer);
                (void)close(output_pipe[0]);
                (void)kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                return -1;
            }
        }

        size_t readable = capacity - used - 1U;
        if (readable > remaining_limit) readable = remaining_limit;
        ssize_t got = read(output_pipe[0], buffer + used, readable);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            (void)close(output_pipe[0]);
            (void)kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        if (got == 0) break;
        used += (size_t)got;
    }

    (void)close(output_pipe[0]);
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            free(buffer);
            return -1;
        }
    }
    buffer[used] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(buffer);
        return -1;
    }
    *output = buffer;
    return 0;
}

static const char *production_mapper_program(void) {
    const char *configured = getenv("LINUX_DEFRAGGER_MAPPER");
    if (configured != NULL && *configured != '\0' &&
        access(configured, X_OK) == 0)
        return configured;
    if (access("/usr/lib/linux-defragger/linux-defragger-mapper", X_OK) == 0)
        return "/usr/lib/linux-defragger/linux-defragger-mapper";
    if (ldtm_program_available("linux-defragger-mapper"))
        return "linux-defragger-mapper";
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

static int remove_flat_directory(const char *path) {
    DIR *directory;
    struct dirent *entry;
    char child[PATH_MAX];
    directory = opendir(path);
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!infiltratr_path_join(child, sizeof(child), path, entry->d_name) || unlink(child) != 0) {
            (void)closedir(directory);
            return -1;
        }
    }
    (void)closedir(directory);
    return rmdir(path);
}

static int sync_path_filesystem(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int result;
    int saved_errno;
    if (fd < 0) return -1;
    result = syncfs(fd);
    saved_errno = errno;
    if (close(fd) != 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }
    errno = saved_errno;
    return result;
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

static int generate_fragmented_data(const LdtmFilesystemSpec *spec, const char *mountpoint,
                                    LdtmTargetRecord records[LDTM_MAX_TARGET_FILES],
                                    size_t *record_count, uint32_t *directory_entries) {
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    char root[PATH_MAX];
    char anchors[PATH_MAX];
    char targets[PATH_MAX];
    char directory_test[PATH_MAX];
    unsigned char *chunk_buffer = NULL;
    int fds[LDTM_MAX_TARGET_FILES];
    EVP_MD_CTX *contexts[LDTM_MAX_TARGET_FILES];
    size_t file_index;
    uint32_t index;
    int result = -1;
    memset(fds, -1, sizeof(fds));
    memset(contexts, 0, sizeof(contexts));
    if (record_count == NULL || directory_entries == NULL || spec == NULL) return -1;
    *record_count = 0U;
    *directory_entries = 0U;
    if (!infiltratr_path_join(root, sizeof(root), mountpoint, "Defragmenter-TestData") ||
        !infiltratr_path_join(anchors, sizeof(anchors), root, "anchors") ||
        !infiltratr_path_join(targets, sizeof(targets), root, "fragmented-files") ||
        !infiltratr_path_join(directory_test, sizeof(directory_test), root, "fragmented-directory")) return -1;
    if (ensure_directory(root, 0755) != 0 || ensure_directory(anchors, 0755) != 0 ||
        ensure_directory(targets, 0755) != 0 || ensure_directory(directory_test, 0755) != 0) return -1;

    printf("Creating %u allocation anchors (%u KiB each)...\n", profile.anchors, profile.anchor_kib);
    fflush(stdout);
    for (index = 0U; index < profile.anchors; ++index) {
        char path[PATH_MAX];
        char name[64];
        (void)snprintf(name, sizeof(name), "anchor-%04u.bin", index);
        if (!infiltratr_path_join(path, sizeof(path), anchors, name) ||
            write_pattern_file(path, (uint64_t)profile.anchor_kib * UINT64_C(1024), index) != 0) goto cleanup;
    }
    for (index = 1U; index < profile.anchors; index += 2U) {
        char path[PATH_MAX];
        char name[64];
        (void)snprintf(name, sizeof(name), "anchor-%04u.bin", index);
        if (!infiltratr_path_join(path, sizeof(path), anchors, name) || unlink(path) != 0) goto cleanup;
    }
    if (sync_path_filesystem(root) != 0) goto cleanup;

    chunk_buffer = malloc((size_t)profile.chunk_kib * 1024U);
    if (chunk_buffer == NULL) goto cleanup;
    for (file_index = 0U; file_index < profile.files && file_index < LDTM_MAX_TARGET_FILES; ++file_index) {
        char path[PATH_MAX];
        char name[64];
        (void)snprintf(name, sizeof(name), "fragmented-%02zu.bin", file_index);
        if (!infiltratr_path_join(path, sizeof(path), targets, name)) goto cleanup;
        fds[file_index] = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fds[file_index] < 0) goto cleanup;
        contexts[file_index] = EVP_MD_CTX_new();
        if (contexts[file_index] == NULL || EVP_DigestInit_ex(contexts[file_index], EVP_sha256(), NULL) != 1) goto cleanup;
    }
    printf("Writing %u target files round-robin in %u x %u KiB chunks...\n",
           profile.files, profile.chunks, profile.chunk_kib);
    fflush(stdout);
    for (index = 0U; index < profile.chunks; ++index) {
        for (file_index = 0U; file_index < profile.files && file_index < LDTM_MAX_TARGET_FILES; ++file_index) {
            const size_t chunk_bytes = (size_t)profile.chunk_kib * 1024U;
            const uint64_t seed = ((uint64_t)file_index << 48) ^ ((uint64_t)index << 16) ^ UINT64_C(0x4c44544d);
            deterministic_fill(chunk_buffer, chunk_bytes, seed);
            if (infiltratr_write_full(fds[file_index], chunk_buffer, chunk_bytes) != 0 ||
                EVP_DigestUpdate(contexts[file_index], chunk_buffer, chunk_bytes) != 1 ||
                fsync(fds[file_index]) != 0) goto cleanup;
        }
        if (index < profile.anchors / 2U) {
            char path[PATH_MAX];
            char name[64];
            (void)snprintf(name, sizeof(name), "interleave-%04u.bin", index);
            if (!infiltratr_path_join(path, sizeof(path), anchors, name) ||
                write_pattern_file(path, ((uint64_t)profile.anchor_kib * UINT64_C(1024)) / 2U,
                                   UINT64_C(0x8000) + index) != 0) goto cleanup;
        }
    }
    for (file_index = 0U; file_index < profile.files && file_index < LDTM_MAX_TARGET_FILES; ++file_index) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_length = 0U;
        if (close(fds[file_index]) != 0) goto cleanup;
        fds[file_index] = -1;
        if (EVP_DigestFinal_ex(contexts[file_index], digest, &digest_length) != 1) goto cleanup;
        EVP_MD_CTX_free(contexts[file_index]);
        contexts[file_index] = NULL;
        (void)snprintf(records[file_index].relative_path,
                       sizeof(records[file_index].relative_path),
                       "fragmented-files/fragmented-%02zu.bin", file_index);
        records[file_index].size = (uint64_t)profile.chunks * (uint64_t)profile.chunk_kib * UINT64_C(1024);
        digest_to_hex(digest, digest_length, records[file_index].sha256);
    }
    *record_count = profile.files < LDTM_MAX_TARGET_FILES ? profile.files : LDTM_MAX_TARGET_FILES;

    printf("Fragmenting directory allocation with a filesystem-sized profile...\n");
    fflush(stdout);
    for (index = 0U; index < profile.directory_initial; ++index) {
        char path[PATH_MAX];
        char name[64];
        int fd;
        char contents[64];
        int length;
        (void)snprintf(name, sizeof(name), "entry-%05u.txt", index);
        if (!infiltratr_path_join(path, sizeof(path), directory_test, name)) goto cleanup;
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) goto cleanup;
        length = snprintf(contents, sizeof(contents), "first %u\n", index);
        if (length < 0 || infiltratr_write_full(fd, contents, (size_t)length) != 0) {
            (void)close(fd);
            goto cleanup;
        }
        if (close(fd) != 0) goto cleanup;
    }
    for (index = 0U; index < profile.directory_initial; index += 2U) {
        char path[PATH_MAX];
        char name[64];
        (void)snprintf(name, sizeof(name), "entry-%05u.txt", index);
        if (!infiltratr_path_join(path, sizeof(path), directory_test, name) || unlink(path) != 0) goto cleanup;
    }
    for (index = 0U; index < profile.directory_second; ++index) {
        char path[PATH_MAX];
        char name[64];
        int fd;
        char contents[64];
        int length;
        const uint32_t entry_index = profile.directory_initial + index;
        (void)snprintf(name, sizeof(name), "entry-%05u.txt", entry_index);
        if (!infiltratr_path_join(path, sizeof(path), directory_test, name)) goto cleanup;
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) goto cleanup;
        length = snprintf(contents, sizeof(contents), "second %u\n", entry_index);
        if (length < 0 || infiltratr_write_full(fd, contents, (size_t)length) != 0) {
            (void)close(fd);
            goto cleanup;
        }
        if (close(fd) != 0) goto cleanup;
    }
    *directory_entries = profile.directory_initial / 2U + profile.directory_second;
    if (sync_path_filesystem(root) != 0) goto cleanup;
    if (remove_flat_directory(anchors) != 0) goto cleanup;

    /*
     * The large round-robin payload stresses fragmentation. These small files
     * deliberately stress allocation and tail-length boundaries that a uniform
     * 256 KiB chunk workload cannot exercise.
     */
    if (generate_edge_case_data(root) != 0) goto cleanup;
    if (sync_path_filesystem(root) != 0) goto cleanup;
    result = 0;

cleanup:
    for (file_index = 0U; file_index < LDTM_MAX_TARGET_FILES; ++file_index) {
        if (fds[file_index] >= 0) (void)close(fds[file_index]);
        if (contexts[file_index] != NULL) EVP_MD_CTX_free(contexts[file_index]);
    }
    free(chunk_buffer);
    return result;
}

static int format_regular(const LdtmFilesystemSpec *spec, const char *partition) {
    const char *const wipe_argv[] = {"wipefs", "--all", "--force", partition, NULL};
    const char *program = ldtm_creator_program(spec);
    if (program == NULL || !ldtm_program_available(program)) return 2;
    if (run_process(wipe_argv, NULL, 0) != 0) return -1;
    switch (spec->creator) {
        case LDTM_CREATOR_FAT12: {
            const char *const argv[] = {program, "-F", "12", "-n", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_FAT16: {
            const char *const argv[] = {program, "-F", "16", "-n", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_FAT32: {
            const char *const argv[] = {program, "-F", "32", "-n", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_EXFAT: {
            const char *const argv[] = {program, "-L", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_NTFS: {
            const char *const argv[] = {program, "-F", "-L", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_EXT2:
        case LDTM_CREATOR_EXT3:
        case LDTM_CREATOR_EXT4: {
            const char *const argv[] = {program, "-F", "-L", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_XFS: {
            /* Keep generated qualification media inside the raw writer's
               deterministic feature contract across xfsprogs versions. */
            const char *const argv[] = {
                program, "-f", "-m", "crc=1,rmapbt=0,reflink=0",
                "-L", spec->label, partition, NULL
            };
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_BTRFS: {
            const char *const argv[] = {program, "-f", "-L", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_AFFS: {
            const char *const argv[] = {program, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_HFS: {
            const char *const argv[] = {program, "-l", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_HFSPLUS: {
            const char *const argv[] = {program, "-v", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_MINIX: {
            const char *const argv[] = {program, "-3", partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_UFS: {
            const char *const argv[] = {program, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_SWAP: {
            const char *const argv[] = {program, "-L", spec->label, partition, NULL};
            return run_process(argv, NULL, 0);
        }
        case LDTM_CREATOR_PFS3:
        case LDTM_CREATOR_ZFS:
        case LDTM_CREATOR_APFS:
        case LDTM_CREATOR_MANUAL:
            break;
    }
    return -1;
}

static int mount_regular(const char *partition, const char *mountpoint, int readonly) {
    if (readonly) {
        const char *const argv[] = {"mount", "-o", "ro", partition, mountpoint, NULL};
        return run_process(argv, NULL, 0);
    }
    {
        const char *const argv[] = {"mount", partition, mountpoint, NULL};
        return run_process(argv, NULL, 0);
    }
}

static void sanitize_tsv(char *text) {
    if (text == NULL) return;
    while (*text != '\0') {
        if (*text == '\t' || *text == '\r' || *text == '\n') *text = ' ';
        ++text;
    }
}

static int state_path_for_device(const char *device, char *path, size_t capacity) {
    const char *base = infiltratr_path_basename(device);
    int count;
    if (base == NULL || *base == '\0' || strchr(base, '/') != NULL)
        return -1;
    count = snprintf(path, capacity, "%s/%s.tsv", LDTM_STATE_ROOT, base);
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

static int state_write_status(FILE *state, const LdtmFilesystemSpec *spec,
                              const char *status, const char *detail) {
    char safe[512];
    if (state == NULL || spec == NULL) return -1;
    (void)snprintf(safe, sizeof(safe), "%s", detail != NULL ? detail : "");
    sanitize_tsv(safe);
    if (fprintf(state, "fs\t%s\t%s\t%s\n", spec->key, status, safe) < 0) return -1;
    return fflush(state);
}

static int state_write_targets(FILE *state, const LdtmFilesystemSpec *spec,
                               const LdtmTargetRecord *records, size_t count,
                               uint32_t directory_entries) {
    size_t index;
    if (fprintf(state, "dircount\t%s\t%u\n", spec->key, directory_entries) < 0) return -1;
    for (index = 0U; index < count; ++index) {
        if (fprintf(state, "file\t%s\t%s\t%llu\t%s\n", spec->key,
                    records[index].relative_path,
                    (unsigned long long)records[index].size,
                    records[index].sha256) < 0) return -1;
    }
    return fflush(state);
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

static int create_regular_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                       const char *work, FILE *state) {
    char mountpoint[PATH_MAX];
    LdtmTargetRecord records[LDTM_MAX_TARGET_FILES];
    size_t record_count = 0U;
    uint32_t directory_entries = 0U;
    int format_result = format_regular(spec, partition);
    if (format_result == 2) {
        emit_status(spec->key, "skipped", "creator is not installed");
        (void)state_write_status(state, spec, "skipped", "creator is not installed");
        return 1;
    }
    if (format_result != 0) {
        emit_status(spec->key, "format-failed", "filesystem creator failed");
        (void)state_write_status(state, spec, "format-failed", "filesystem creator failed");
        return 1;
    }
    if (spec->payload_mib == 0U) {
        char detail[512];
        if (spec->creator != LDTM_CREATOR_SWAP ||
            production_map_accepts(
                spec, partition, detail, sizeof(detail)) != 0) {
            emit_status(
                spec->key, "qualification-failed",
                spec->creator == LDTM_CREATOR_SWAP
                    ? detail
                    : "filesystem has no retained payload qualification contract");
            (void)state_write_status(
                state, spec, "qualification-failed",
                spec->creator == LDTM_CREATOR_SWAP
                    ? detail
                    : "no retained payload qualification contract");
            return 1;
        }
        emit_status(
            spec->key, "populated",
            "swap header/page-map geometry accepted by the production analyser; file fragmentation is not applicable");
        (void)state_write_status(
            state, spec, "populated",
            "swap header/page-map geometry accepted by the production analyser; file fragmentation is not applicable");
        return 0;
    }
    if (snprintf(mountpoint, sizeof(mountpoint), "%s/%s", work, spec->key) <= 0 ||
        ensure_directory(mountpoint, 0755) != 0) return -1;
    if (mount_regular(partition, mountpoint, 0) != 0) {
        emit_status(spec->key, "formatted-unpopulated", "formatted successfully but host could not mount it read/write");
        (void)state_write_status(state, spec, "formatted-unpopulated", "host mount failed");
        return 1;
    }
    if (generate_fragmented_data(spec, mountpoint, records, &record_count, &directory_entries) != 0) {
        const char *const umount_argv[] = {"umount", mountpoint, NULL};
        (void)run_process(umount_argv, NULL, 0);
        emit_status(spec->key, "formatted-unpopulated", "fragmentation payload generation failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "payload generation failed");
        return 1;
    }
    {
        const char *const umount_argv[] = {"umount", mountpoint, NULL};
        if (run_process(umount_argv, NULL, 0) != 0) return -1;
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
    if (state_write_status(
            state, spec, "populated",
            "fragmented deterministic payload created and production analyser proved fragmentation") != 0 ||
        state_write_targets(state, spec, records, record_count, directory_entries) != 0)
        return -1;
    emit_status(
        spec->key, "populated",
        "fragmented deterministic payload created and production analyser proved fragmentation");
    return 0;
}

static void short_device_hash(const char *device, char output[17]) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0U;
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(context, device, strlen(device)) != 1 ||
        EVP_DigestFinal_ex(context, digest, &length) != 1 || length < 8U) {
        (void)snprintf(output, 17U, "0000000000000000");
    } else {
        char full[LDTM_HASH_HEX];
        digest_to_hex(digest, length, full);
        memcpy(output, full, 16U);
        output[16] = '\0';
    }
    if (context != NULL) EVP_MD_CTX_free(context);
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
    LdUfsSummary summary;
    char error[256];
    return ufs_read_summary(path, &summary, error, sizeof(error)) == 0 &&
           summary.variant == LD_UFS_VARIANT_UFS2_LE;
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

static int verify_ufs2_raw_payload(
    const char *path, const LdtmTargetRecord *targets,
    size_t target_count, uint32_t expected_directory_entries,
    char *detail, size_t detail_capacity)
{
    LdtmUfs2Geometry geometry;
    uint8_t *cg = NULL;
    uint8_t inode[LDTM_UFS2_INODE_SIZE];
    uint8_t matched[LDTM_MAX_TARGET_FILES] = {0};
    uint64_t regular_files = 0U;
    int fd = -1;
    int result = -1;

    if (detail != NULL && detail_capacity > 0U)
        detail[0] = '\0';
    if (path == NULL || targets == NULL ||
        target_count > LDTM_MAX_TARGET_FILES)
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
    if (regular_files !=
        (uint64_t)target_count +
            expected_directory_entries)
        goto cleanup;

    if (detail != NULL && detail_capacity > 0U)
        (void)snprintf(
            detail, detail_capacity,
            "independent raw UFS2 verifier matched %zu retained SHA-256 payloads and %u directory-test files",
            target_count, expected_directory_entries);
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

static int zfs_exact_analysis_ok(const char *path) {
    LdZfsAnalysis analysis;
    char error[256] = {0};
    if (zfs_analyse_exact(path, &analysis, error, sizeof(error)) != 0)
        return 0;
    const int ok = analysis.exact_allocation && analysis.exact_fragmentation &&
                   analysis.unknown_bytes == 0U;
    zfs_analysis_destroy(&analysis);
    return ok;
}

static int create_ufs_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   const char *work, FILE *state) {
    char source[PATH_MAX];
    char image[PATH_MAX];
    LdtmTargetRecord records[LDTM_MAX_TARGET_FILES];
    size_t record_count = 0U;
    uint32_t directory_entries = 0U;
    const char *const wipe_argv[] = {"wipefs", "--all", "--force", partition, NULL};
    const char *makefs_argv[12];
    char image_size[32];
    if (!ldtm_program_available("makefs")) {
        emit_status(spec->key, "skipped", "makefs is not installed");
        (void)state_write_status(state, spec, "skipped", "makefs is not installed");
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
    if (generate_fragmented_data(spec, source, records, &record_count, &directory_entries) != 0) {
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
    if (snprintf(image_size, sizeof(image_size), "%um",
                 spec->size_mib) <= 0)
        return -1;
    makefs_argv[6] = image_size;
    makefs_argv[7] = "-o";
    makefs_argv[8] =
        "version=2,bsize=8192,fsize=1024,minfree=5,maxbpg=256,optimization=space";
    makefs_argv[9] = image;
    makefs_argv[10] = source;
    makefs_argv[11] = NULL;
    if (run_process(makefs_argv, NULL, 0) != 0 || !ufs2_summary_ok(image)) {
        emit_status(spec->key, "format-failed", "makefs did not produce a recognised UFS2 image");
        (void)state_write_status(state, spec, "format-failed", "makefs UFS2 validation failed");
        return 1;
    }
    printf("+ copy verified UFS2 image %s -> %s\n", image, partition);
    fflush(stdout);
    if (copy_image_to_partition(image, partition) != 0 ||
        !ufs2_summary_ok(partition)) {
        emit_status(
            spec->key, "format-failed",
            "UFS2 image copy could not be independently validated");
        (void)state_write_status(
            state, spec, "format-failed",
            "UFS2 partition validation failed");
        return 1;
    }
    {
        char detail[512] = {0};
        if (verify_ufs2_raw_payload(
                partition, records, record_count,
                directory_entries, detail,
                sizeof(detail)) != 0) {
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
    if (state_write_status(
            state, spec, "populated",
            "deterministic UFS2 payload created at the capped media size and production analyser proved fragmentation") != 0 ||
        state_write_targets(state, spec, records, record_count, directory_entries) != 0)
        return -1;
    emit_status(
        spec->key, "populated",
        "deterministic UFS2 payload created at the capped media size and production analyser proved fragmentation");
    return 0;
}

static int create_zfs_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   const char *work, const char *device, FILE *state) {
    char pool_hash[17];
    char pool[64];
    char altroot[PATH_MAX];
    char mountpoint[PATH_MAX];
    LdtmTargetRecord records[LDTM_MAX_TARGET_FILES];
    size_t record_count = 0U;
    uint32_t directory_entries = 0U;
    const char *const wipe_argv[] = {"wipefs", "--all", "--force", partition, NULL};
    if (!ldtm_program_available("zpool")) {
        emit_status(spec->key, "skipped", "zpool is not installed");
        (void)state_write_status(state, spec, "skipped", "zpool is not installed");
        return 1;
    }
    short_device_hash(device, pool_hash);
    (void)snprintf(pool, sizeof(pool), "ldtest_%.16s", pool_hash);
    if (snprintf(altroot, sizeof(altroot), "%s/zfs-root", work) <= 0 ||
        ensure_directory(altroot, 0755) != 0 ||
        snprintf(mountpoint, sizeof(mountpoint), "%s/ldtest", altroot) <= 0) return -1;
    if (run_process(wipe_argv, NULL, 0) != 0) return -1;
    {
        const char *const argv[] = {
            "zpool", "create", "-f", "-R", altroot, "-m", "/ldtest",
            "-o", "cachefile=none", "-o", "version=28",
            pool, partition, NULL
        };
        if (run_process(argv, NULL, 0) != 0) {
            emit_status(spec->key, "format-failed", "zpool create failed");
            (void)state_write_status(state, spec, "format-failed", "zpool create failed");
            return 1;
        }
    }
    if (generate_fragmented_data(spec, mountpoint, records, &record_count, &directory_entries) != 0) {
        const char *const export_argv[] = {"zpool", "export", pool, NULL};
        (void)run_process(export_argv, NULL, 0);
        emit_status(spec->key, "formatted-unpopulated", "ZFS payload generation failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "ZFS payload generation failed");
        return 1;
    }
    {
        const char *const export_argv[] = {"zpool", "export", pool, NULL};
        if (run_process(export_argv, NULL, 0) != 0) return -1;
    }
    if (!zfs_exact_analysis_ok(partition)) {
        emit_status(spec->key, "format-failed",
                    "native exact analyser rejected the exported ZFS v28 pool");
        (void)state_write_status(
            state, spec, "format-failed",
            "native exact analyser rejected the exported ZFS v28 pool");
        return 1;
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
    if (state_write_status(
            state, spec, "populated",
            "deterministic ZFS v28 payload created; exact production analyser proved real fragmentation") != 0 ||
        fprintf(state, "pool\t%s\t%s\n", spec->key, pool) < 0 ||
        fflush(state) != 0 ||
        state_write_targets(state, spec, records, record_count,
                            directory_entries) != 0)
        return -1;
    emit_status(
        spec->key, "populated",
        "deterministic ZFS v28 payload created; exact production analyser proved real fragmentation");
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
    char work_template[] = "/tmp/linux-defragger-test-media.XXXXXX";
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
    if (fprintf(state, "schema\t2\ndevice\t%s\nfingerprint\t%s\n",
                canonical, confirmed_fingerprint) < 0 ||
        fflush(state) != 0)
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
            } else if (spec->creator == LDTM_CREATOR_ZFS) {
                filesystem_result =
                    create_zfs_and_populate(
                        spec, partition, work, canonical, state);
            } else {
                filesystem_result =
                    create_regular_and_populate(
                        spec, partition, work, state);
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

static int hash_file(const char *path, uint64_t expected_size, const char *expected_hash) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    EVP_MD_CTX *context = NULL;
    unsigned char buffer[65536];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0U;
    uint64_t total = 0U;
    char hex[LDTM_HASH_HEX];
    int result = -1;
    if (fd < 0) return -1;
    context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) goto cleanup;
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            goto cleanup;
        }
        if (got == 0) break;
        total += (uint64_t)got;
        if (EVP_DigestUpdate(context, buffer, (size_t)got) != 1) goto cleanup;
    }
    if (EVP_DigestFinal_ex(context, digest, &digest_length) != 1) goto cleanup;
    digest_to_hex(digest, digest_length, hex);
    result = total == expected_size && strcmp(hex, expected_hash) == 0 ? 0 : -1;
cleanup:
    if (context != NULL) EVP_MD_CTX_free(context);
    (void)close(fd);
    return result;
}

static int file_matches_text(const char *path, const char *expected,
                             size_t expected_length) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    char buffer[128];
    size_t done = 0U;
    int result = -1;
    if (fd < 0 || expected_length > sizeof(buffer))
        goto cleanup;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != (uint64_t)expected_length)
        goto cleanup;
    while (done < expected_length) {
        ssize_t got = read(fd, buffer + done, expected_length - done);
        if (got < 0) {
            if (errno == EINTR) continue;
            goto cleanup;
        }
        if (got == 0) goto cleanup;
        done += (size_t)got;
    }
    if (memcmp(buffer, expected, expected_length) != 0)
        goto cleanup;
    {
        char extra;
        ssize_t got;
        do {
            got = read(fd, &extra, 1U);
        } while (got < 0 && errno == EINTR);
        if (got != 0) goto cleanup;
    }
    result = 0;
cleanup:
    if (fd >= 0) (void)close(fd);
    return result;
}

static int verify_fragmented_directory_payload(
    const LdtmFilesystemSpec *spec, const char *directory_path) {
    const LdtmFragmentProfile profile = ldtm_fragment_profile(spec);
    uint32_t index;

    for (index = 1U; index < profile.directory_initial; index += 2U) {
        char path[PATH_MAX];
        char name[64];
        char contents[64];
        int length;
        (void)snprintf(name, sizeof(name), "entry-%05u.txt", index);
        if (!infiltratr_path_join(path, sizeof(path), directory_path, name))
            return -1;
        length = snprintf(contents, sizeof(contents), "first %u\n", index);
        if (length < 0 || (size_t)length >= sizeof(contents) ||
            file_matches_text(path, contents, (size_t)length) != 0)
            return -1;
    }
    for (index = 0U; index < profile.directory_second; ++index) {
        const uint32_t entry_index = profile.directory_initial + index;
        char path[PATH_MAX];
        char name[64];
        char contents[64];
        int length;
        (void)snprintf(name, sizeof(name), "entry-%05u.txt", entry_index);
        if (!infiltratr_path_join(path, sizeof(path), directory_path, name))
            return -1;
        length = snprintf(contents, sizeof(contents), "second %u\n", entry_index);
        if (length < 0 || (size_t)length >= sizeof(contents) ||
            file_matches_text(path, contents, (size_t)length) != 0)
            return -1;
    }
    return 0;
}

static uint32_t directory_entry_count(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    uint32_t count = 0U;
    if (directory == NULL) return UINT32_MAX;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) ++count;
    }
    (void)closedir(directory);
    return count;
}

static int verify_edge_pattern_file(const char *path, uint32_t size,
                                    uint64_t seed) {
    unsigned char actual[8192];
    unsigned char expected[8192];
    struct stat st;
    int fd;
    int result = -1;

    if ((size_t)size > sizeof(actual))
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || (uint64_t)st.st_size != (uint64_t)size)
        goto cleanup;
    if (size > 0U) {
        if (infiltratr_pread_full(fd, actual, (size_t)size, 0U) != 0)
            goto cleanup;
        deterministic_fill(expected, (size_t)size, seed);
        if (memcmp(actual, expected, (size_t)size) != 0)
            goto cleanup;
    }
    result = 0;
cleanup:
    (void)close(fd);
    return result;
}

static int verify_edge_case_data(const char *root) {
    char directory[PATH_MAX];
    size_t index;
    if (!infiltratr_path_join(directory, sizeof(directory), root, "edge-cases") ||
        directory_entry_count(directory) != (uint32_t)LDTM_EDGE_CASE_COUNT)
        return -1;
    for (index = 0U; index < LDTM_EDGE_CASE_COUNT; ++index) {
        char path[PATH_MAX];
        char name[64];
        const uint32_t size = ldtm_edge_case_sizes[index];
        (void)snprintf(name, sizeof(name), "edge-%02zu-%u.bin", index, size);
        if (!infiltratr_path_join(path, sizeof(path), directory, name) ||
            verify_edge_pattern_file(path, size, edge_case_seed(index, size)) != 0)
            return -1;
    }
    return 0;
}

static int load_verify_state(const char *state_path, const char *device,
                             LdtmVerifyFilesystem state[LDTM_SPEC_COUNT]) {
    FILE *stream = open_state_stream(state_path, 0);
    char line[LDTM_LINE_MAX];
    char saved_fingerprint[LDTM_HASH_HEX] = "";
    char current_fingerprint[LDTM_HASH_HEX] = "";
    uint64_t schema = 0U;
    if (stream == NULL) return -1;
    memset(state, 0, sizeof(LdtmVerifyFilesystem) * LDTM_SPEC_COUNT);
    while (fgets(line, sizeof(line), stream) != NULL) {
        char *fields[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
        size_t field_count = 0U;
        char *cursor = line;
        char *token;
        char *saveptr = NULL;
        infiltratr_trim(cursor);
        token = strtok_r(cursor, "\t", &saveptr);
        while (token != NULL && field_count < 6U) {
            fields[field_count++] = token;
            token = strtok_r(NULL, "\t", &saveptr);
        }
        if (field_count < 2U) continue;
        if (strcmp(fields[0], "schema") == 0) {
            if (!infiltratr_parse_u64(fields[1], 10U, &schema)) {
                (void)fclose(stream);
                return -1;
            }
        } else if (strcmp(fields[0], "fingerprint") == 0) {
            if (strlen(fields[1]) != LDTM_HASH_HEX - 1U) {
                (void)fclose(stream);
                return -1;
            }
            (void)snprintf(saved_fingerprint, sizeof(saved_fingerprint),
                           "%s", fields[1]);
        } else if (strcmp(fields[0], "fs") == 0 && field_count >= 3U) {
            size_t index;
            for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
                if (strcmp(ldtm_specs()[index].key, fields[1]) == 0 && strcmp(fields[2], "populated") == 0) {
                    state[index].populated = 1;
                }
            }
        } else if (strcmp(fields[0], "pool") == 0 && field_count >= 3U) {
            size_t index;
            for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
                if (strcmp(ldtm_specs()[index].key, fields[1]) == 0) {
                    (void)snprintf(state[index].pool, sizeof(state[index].pool), "%s", fields[2]);
                }
            }
        } else if (strcmp(fields[0], "dircount") == 0 && field_count >= 3U) {
            size_t index;
            for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
                if (strcmp(ldtm_specs()[index].key, fields[1]) == 0) {
                    uint64_t parsed = 0U;
                    if (!infiltratr_parse_u64_range(fields[2], 10U, 0U,
                                                    UINT32_MAX, &parsed)) {
                        (void)fclose(stream);
                        return -1;
                    }
                    state[index].directory_entries = (uint32_t)parsed;
                }
            }
        } else if (strcmp(fields[0], "file") == 0 && field_count >= 5U) {
            size_t index;
            for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
                LdtmVerifyFilesystem *filesystem = &state[index];
                if (strcmp(ldtm_specs()[index].key, fields[1]) == 0 &&
                    filesystem->target_count < LDTM_MAX_TARGET_FILES) {
                    uint64_t parsed_size = 0U;
                    if (!infiltratr_parse_u64(fields[3], 10U, &parsed_size)) {
                        (void)fclose(stream);
                        return -1;
                    }
                    LdtmTargetRecord *record = &filesystem->targets[filesystem->target_count++];
                    (void)snprintf(record->relative_path, sizeof(record->relative_path), "%s", fields[2]);
                    record->size = parsed_size;
                    (void)snprintf(record->sha256, sizeof(record->sha256), "%s", fields[4]);
                }
            }
        }
    }
    (void)fclose(stream);
    if (schema != 2U || saved_fingerprint[0] == '\0' ||
        ldtm_device_fingerprint(device, current_fingerprint) != 0 ||
        strcmp(saved_fingerprint, current_fingerprint) != 0)
        return -1;
    return 0;
}

static int verify_mounted_payload(const LdtmFilesystemSpec *spec, const char *mountpoint,
                                  const LdtmVerifyFilesystem *expected) {
    size_t index;
    char root[PATH_MAX];
    char directory_path[PATH_MAX];
    if (!infiltratr_path_join(root, sizeof(root), mountpoint, "Defragmenter-TestData")) return -1;
    for (index = 0U; index < expected->target_count; ++index) {
        char path[PATH_MAX];
        if (!infiltratr_path_join(path, sizeof(path), root, expected->targets[index].relative_path) ||
            hash_file(path, expected->targets[index].size, expected->targets[index].sha256) != 0) {
            char detail[256];
            (void)snprintf(detail, sizeof(detail), "checksum/size mismatch: %s",
                           expected->targets[index].relative_path);
            emit_status(spec->key, "verify-failed", detail);
            return -1;
        }
    }
    if (!infiltratr_path_join(directory_path, sizeof(directory_path), root,
                              "fragmented-directory") ||
        directory_entry_count(directory_path) != expected->directory_entries) {
        emit_status(spec->key, "verify-failed", "directory-entry count changed");
        return -1;
    }
    if (verify_fragmented_directory_payload(spec, directory_path) != 0) {
        emit_status(spec->key, "verify-failed",
                    "retained directory payload content changed");
        return -1;
    }
    if (verify_edge_case_data(root) != 0) {
        emit_status(spec->key, "verify-failed",
                    "boundary-sized payload files changed");
        return -1;
    }
    emit_status(spec->key, "verified",
                "all retained hashes, directory data and boundary-sized payload bytes match");
    return 0;
}

static int verify_zfs(const LdtmFilesystemSpec *spec, const char *work,
                      const LdtmVerifyFilesystem *expected) {
    char altroot[PATH_MAX];
    char mountpoint[PATH_MAX];
    if (*expected->pool == '\0' || !ldtm_program_available("zpool") || !ldtm_program_available("zfs")) {
        emit_status(spec->key, "verify-failed", "ZFS verification tools are unavailable");
        return -1;
    }
    if (snprintf(altroot, sizeof(altroot), "%s/zfs-verify", work) <= 0 ||
        ensure_directory(altroot, 0755) != 0 ||
        snprintf(mountpoint, sizeof(mountpoint), "%s/ldtest", altroot) <= 0) return -1;
    {
        const char *const import_argv[] = {
            "zpool", "import", "-d", "/dev", "-N", "-o", "readonly=on", "-R", altroot,
            expected->pool, NULL
        };
        if (run_process(import_argv, NULL, 0) != 0) {
            emit_status(spec->key, "verify-failed", "host could not import ZFS pool read-only");
            return -1;
        }
    }
    {
        const char *const mount_argv[] = {"zfs", "mount", expected->pool, NULL};
        if (run_process(mount_argv, NULL, 0) != 0) {
            const char *const export_argv[] = {"zpool", "export", expected->pool, NULL};
            (void)run_process(export_argv, NULL, 0);
            emit_status(spec->key, "verify-failed", "host could not mount imported ZFS pool");
            return -1;
        }
    }
    const int payload_result =
        verify_mounted_payload(spec, mountpoint, expected);
    {
        const char *const export_argv[] = {"zpool", "export", expected->pool, NULL};
        if (run_process(export_argv, NULL, 0) != 0)
            return -1;
    }
    return payload_result;
}

int ldtm_worker_verify(const char *device) {
    char canonical[PATH_MAX];
    char safety[512];
    char state_path[PATH_MAX];
    char work_template[] = "/tmp/linux-defragger-test-media-verify.XXXXXX";
    char *work;
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
    work = mkdtemp(work_template);
    if (work == NULL)
        return 2;

    for (index = 0U; index < LDTM_SPEC_COUNT; ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        const char *partition;
        char mountpoint[PATH_MAX];
        if (!expected[index].populated)
            continue;
        printf("\n=== verify %s ===\n", spec->key);
        fflush(stdout);

        partition = partition_for_label(&map, spec->label);
        if (partition == NULL) {
            emit_status(spec->key, "verify-failed",
                        "partition label is missing");
            failures++;
            continue;
        }

        /*
         * ZFS is intentionally analysis-only in Defragmenter.  It therefore
         * keeps its pre-existing fragmented layout, but its retained payload
         * still has to survive byte-for-byte and the exact analyser must still
         * accept the pool.
         */
        if (spec->creator == LDTM_CREATOR_ZFS) {
            if (!zfs_exact_analysis_ok(partition) ||
                verify_zfs(spec, work, &expected[index]) != 0)
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
            if (production_map_accepts(
                    spec, partition, detail, sizeof(detail)) != 0) {
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
            if (require_fragmentation_state(
                    spec, partition, 0,
                    detail, sizeof(detail)) != 0 ||
                verify_ufs2_raw_payload(
                    partition, expected[index].targets,
                    expected[index].target_count,
                    expected[index].directory_entries,
                    detail, sizeof(detail)) != 0) {
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
            if (require_fragmentation_state(
                    spec, partition, 0,
                    detail, sizeof(detail)) != 0) {
                emit_status(spec->key, "verify-failed", detail);
                failures++;
                continue;
            }
        }

        if (snprintf(mountpoint, sizeof(mountpoint), "%s/%s",
                     work, spec->key) <= 0 ||
            ensure_directory(mountpoint, 0755) != 0) {
            emit_status(spec->key, "verify-failed",
                        "could not create verification mountpoint");
            failures++;
            continue;
        }
        if (mount_regular(partition, mountpoint, 1) != 0) {
            emit_status(
                spec->key, "verify-failed",
                "host kernel could not mount this qualified filesystem read-only for independent payload verification");
            failures++;
            continue;
        }
        if (verify_mounted_payload(
                spec, mountpoint, &expected[index]) != 0)
            failures++;
        {
            const char *const umount_argv[] = {
                "umount", mountpoint, NULL
            };
            if (run_process(umount_argv, NULL, 0) != 0) {
                emit_status(spec->key, "verify-failed",
                            "verification mount could not be cleanly unmounted");
                failures++;
            }
        }
    }
    (void)recursive_remove(work);
    if (failures != 0) {
        fprintf(stderr,
                "Test-media verification failed closed: %d filesystem qualification failure%s.\n",
                failures, failures == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
