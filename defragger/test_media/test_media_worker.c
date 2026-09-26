// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "ufs_native.h"
#include "zfs_native.h"

#include "infiltratr/arithmetic.h"
#include "infiltratr/core.h"
#include "infiltratr/posix_path.h"
#include "infiltratr/posix.h"
#include "infiltratr/posix_io.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
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

typedef struct {
    int populated;
    char pool[96];
    LdtmTargetRecord targets[LDTM_MAX_TARGET_FILES];
    size_t target_count;
    uint32_t directory_entries;
} LdtmVerifyFilesystem;

static void emit_status(const char *filesystem, const char *status, const char *detail) {
    const char *safe_detail = detail != NULL ? detail : "";
    printf("LDTM_STATUS\t%s\t%s\t%s\n", filesystem, status, safe_detail);
    fflush(stdout);
}

static int run_process(const char *const argv[], const char *stdin_text, int quiet) {
    int input_pipe[2] = {-1, -1};
    pid_t child;
    int status = 0;
    if (argv == NULL || argv[0] == NULL) return -1;
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
        execvp(argv[0], (char *const *)argv);
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
    if (output == NULL || argv == NULL || argv[0] == NULL) return -1;
    *output = NULL;
    if (pipe(output_pipe) != 0) return -1;
    child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }
    if (child == 0) {
        (void)close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        (void)close(output_pipe[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    (void)close(output_pipe[1]);
    if (!infiltratr_array_reserve((void **)&buffer, &capacity, 1U, 4096U, 4096U)) {
        (void)close(output_pipe[0]);
        (void)waitpid(child, &status, 0);
        return -1;
    }
    for (;;) {
        ssize_t got;
        if (capacity - used < 2048U) {
            size_t required = 0U;
            if (!infiltratr_size_add_checked(used, 2049U, &required) ||
                !infiltratr_array_reserve((void **)&buffer, &capacity, 1U,
                                          required, 4096U)) {
                free(buffer);
                (void)close(output_pipe[0]);
                (void)waitpid(child, &status, 0);
                return -1;
            }
        }
        got = read(output_pipe[0], buffer + used, capacity - used - 1U);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            (void)close(output_pipe[0]);
            (void)waitpid(child, &status, 0);
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

static int parse_json_u64_field(const char *json, const char *name,
                                uint64_t *value) {
    char needle[96];
    const char *field;
    const char *cursor;
    char number[32];
    size_t used = 0U;
    if (json == NULL || name == NULL || value == NULL ||
        snprintf(needle, sizeof(needle), "\"%s\"", name) <= 0)
        return -1;
    field = strstr(json, needle);
    if (field == NULL || (cursor = strchr(field, ':')) == NULL)
        return -1;
    ++cursor;
    while (*cursor != '\0' && isspace((unsigned char)*cursor))
        ++cursor;
    while (*cursor >= '0' && *cursor <= '9' &&
           used + 1U < sizeof(number))
        number[used++] = *cursor++;
    number[used] = '\0';
    return used != 0U &&
           infiltratr_parse_u64(number, 10U, value) ? 0 : -1;
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

int ldtm_is_whole_block_device(const char *device) {
    struct stat st;
    const char *const argv[] = {"lsblk", "-d", "-n", "-o", "TYPE", "--", device, NULL};
    char *output = NULL;
    int result;
    if (device == NULL || stat(device, &st) != 0 || !S_ISBLK(st.st_mode)) return 0;
    if (capture_process(argv, &output) != 0) return 0;
    infiltratr_trim(output);
    result = strcmp(output, "disk") == 0;
    free(output);
    return result;
}

static int mount_source_disk(const char *mountpoint, char *disk, size_t capacity) {
    const char *const findmnt_argv[] = {"findmnt", "-n", "-o", "SOURCE", "--target", mountpoint, NULL};
    char *source = NULL;
    char *ancestry = NULL;
    char *line;
    char *saveptr = NULL;
    int found = 0;
    if (capture_process(findmnt_argv, &source) != 0) return 0;
    infiltratr_trim(source);
    if (!infiltratr_string_starts_with(source, "/dev/")) {
        free(source);
        return 0;
    }
    {
        const char *const lsblk_argv[] = {"lsblk", "-s", "-n", "-p", "-o", "PATH,TYPE", "--", source, NULL};
        if (capture_process(lsblk_argv, &ancestry) != 0) {
            free(source);
            return 0;
        }
    }
    line = strtok_r(ancestry, "\n", &saveptr);
    while (line != NULL) {
        char path[PATH_MAX];
        char type[32];
        if (sscanf(line, "%4095s %31s", path, type) == 2 && strcmp(type, "disk") == 0) {
            char canonical[PATH_MAX];
            if (ldtm_canonicalize_device(path, canonical, sizeof(canonical)) == 0 &&
                strlen(canonical) + 1U <= capacity) {
                memcpy(disk, canonical, strlen(canonical) + 1U);
                found = 1;
                break;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(ancestry);
    free(source);
    return found;
}

int ldtm_is_system_disk(const char *device) {
    static const char *const mountpoints[] = {"/", "/boot", "/boot/efi"};
    size_t index;
    char canonical[PATH_MAX];
    if (ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0) return 1;
    for (index = 0U; index < sizeof(mountpoints) / sizeof(mountpoints[0]); ++index) {
        char disk[PATH_MAX];
        if (mount_source_disk(mountpoints[index], disk, sizeof(disk)) &&
            strcmp(canonical, disk) == 0) return 1;
    }
    return 0;
}

int ldtm_device_safety_check(const char *device, int allow_non_removable,
                             char *detail, size_t detail_capacity) {
    char canonical[PATH_MAX];
    const char *const argv[] = {
        "lsblk", "-d", "-b", "-n", "-P", "-o", "SIZE,RM,RO,TRAN", "--", device, NULL
    };
    char *output = NULL;
    char size_text[64] = "0";
    char rm_text[16] = "0";
    char ro_text[16] = "0";
    char transport[64] = "";
    uint64_t bytes = 0U;
    uint64_t removable_value = 0U;
    uint64_t readonly_value = 0U;
    int removable;
    int readonly;
    if (detail == NULL || detail_capacity == 0U) return -1;
    detail[0] = '\0';
    if (ldtm_canonicalize_device(device, canonical, sizeof(canonical)) != 0) {
        (void)snprintf(detail, detail_capacity, "Target does not exist: %s", device);
        return -1;
    }
    if (!ldtm_is_whole_block_device(canonical)) {
        (void)snprintf(detail, detail_capacity, "Refusing non-whole-disk target: %s", canonical);
        return -1;
    }
    if (ldtm_is_system_disk(canonical)) {
        (void)snprintf(detail, detail_capacity, "Refusing system/boot disk: %s", canonical);
        return -1;
    }
    if (capture_process(argv, &output) != 0) {
        (void)snprintf(detail, detail_capacity, "Unable to inspect %s", canonical);
        return -1;
    }
    (void)extract_pair(output, "SIZE", size_text, sizeof(size_text));
    (void)extract_pair(output, "RM", rm_text, sizeof(rm_text));
    (void)extract_pair(output, "RO", ro_text, sizeof(ro_text));
    (void)extract_pair(output, "TRAN", transport, sizeof(transport));
    free(output);
    if (!infiltratr_parse_u64(size_text, 10U, &bytes) ||
        !infiltratr_parse_u64_range(rm_text, 10U, 0U, 1U, &removable_value) ||
        !infiltratr_parse_u64_range(ro_text, 10U, 0U, 1U, &readonly_value)) {
        (void)snprintf(detail, detail_capacity,
                       "Unable to parse target properties for %s", canonical);
        return -1;
    }
    removable = (int)removable_value;
    readonly = (int)readonly_value;
    if (readonly != 0) {
        (void)snprintf(detail, detail_capacity, "Refusing read-only target: %s", canonical);
        return -1;
    }
    if (bytes < ldtm_required_capacity_bytes()) {
        (void)snprintf(detail, detail_capacity,
                       "Target is too small: %.1f GiB; need at least %.1f GiB",
                       (double)bytes / (double)LDTM_GIB,
                       (double)ldtm_required_capacity_bytes() / (double)LDTM_GIB);
        return -1;
    }
    if (!allow_non_removable && !ldtm_transport_is_field_media(removable, transport)) {
        (void)snprintf(detail, detail_capacity,
                       "Refusing non-removable target by default (RM=%d, TRAN=%s)",
                       removable, *transport != '\0' ? transport : "unknown");
        return -1;
    }
    (void)snprintf(detail, detail_capacity, "Safe field-media target: %s", canonical);
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

static int settle_partitions(const char *device) {
    const char *const partx_argv[] = {"partx", "-u", device, NULL};
    const char *const udev_argv[] = {"udevadm", "settle", NULL};
    (void)run_process(partx_argv, NULL, 0);
    if (ldtm_program_available("udevadm")) (void)run_process(udev_argv, NULL, 0);
    sleep(1U);
    return 0;
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
    sync();

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
    sync();
    if (remove_flat_directory(anchors) != 0) goto cleanup;
    sync();
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
    count = snprintf(path, capacity, "%s/%s.tsv", LDTM_STATE_ROOT, base);
    return count < 0 || (size_t)count >= capacity ? -1 : 0;
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
        return 0;
    }
    printf("+ built-in raw C Amiga DOS\\%u formatter/populator %s\n", dostype, partition);
    fflush(stdout);
    if (ldtm_format_amiga_volume(partition, dostype, spec->label) != 0) {
        emit_status(spec->key, "format-failed", "built-in C Amiga formatter failed");
        (void)state_write_status(state, spec, "format-failed", "built-in C formatter failed");
        return 0;
    }
    if (ldtm_populate_amiga_volume(partition, dostype, &profile) != 0) {
        emit_status(spec->key, "formatted-unpopulated", "raw C Amiga fragmentation payload generation failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "raw C payload generation failed");
        return 0;
    }
    detail[0] = '\0';
    if (ldtm_verify_amiga_payload(partition, dostype, &profile, detail, sizeof(detail)) != 0) {
        emit_status(spec->key, "formatted-unpopulated",
                    detail[0] != '\0' ? detail : "raw C Amiga payload self-check failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "raw C payload self-check failed");
        return 0;
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
            return 0;
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
        return 0;
    }
    if (format_result != 0) {
        emit_status(spec->key, "format-failed", "filesystem creator failed");
        (void)state_write_status(state, spec, "format-failed", "filesystem creator failed");
        return 0;
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
            return 0;
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
        return 0;
    }
    if (generate_fragmented_data(spec, mountpoint, records, &record_count, &directory_entries) != 0) {
        const char *const umount_argv[] = {"umount", mountpoint, NULL};
        (void)run_process(umount_argv, NULL, 0);
        emit_status(spec->key, "formatted-unpopulated", "fragmentation payload generation failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "payload generation failed");
        return 0;
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
            return 0;
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
        return 0;
    }
    if (run_process(wipe_argv, NULL, 0) != 0 ||
        snprintf(source, sizeof(source), "%s/ufs-source", work) <= 0 ||
        snprintf(image, sizeof(image), "%s/ufs.img", work) <= 0 ||
        ensure_directory(source, 0755) != 0) {
        emit_status(spec->key, "format-failed", "could not prepare UFS image workspace");
        (void)state_write_status(state, spec, "format-failed", "UFS workspace preparation failed");
        return 0;
    }
    if (generate_fragmented_data(spec, source, records, &record_count, &directory_entries) != 0) {
        emit_status(spec->key, "format-failed", "could not build deterministic UFS source tree");
        (void)state_write_status(state, spec, "format-failed", "UFS source tree generation failed");
        return 0;
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
    makefs_argv[8] = "version=2,bsize=8192,fsize=1024,minfree=5";
    makefs_argv[9] = image;
    makefs_argv[10] = source;
    makefs_argv[11] = NULL;
    if (run_process(makefs_argv, NULL, 0) != 0 || !ufs2_summary_ok(image)) {
        emit_status(spec->key, "format-failed", "makefs did not produce a recognised UFS2 image");
        (void)state_write_status(state, spec, "format-failed", "makefs UFS2 validation failed");
        return 0;
    }
    printf("+ copy verified UFS2 image %s -> %s\n", image, partition);
    fflush(stdout);
    if (copy_image_to_partition(image, partition) != 0 || !ufs2_summary_ok(partition)) {
        emit_status(spec->key, "format-failed", "UFS2 image copy could not be independently validated");
        (void)state_write_status(state, spec, "format-failed", "UFS2 partition validation failed");
        return 0;
    }
    {
        char detail[512];
        if (require_fragmentation_state(
                spec, partition, 1, detail, sizeof(detail)) != 0) {
            emit_status(spec->key, "qualification-failed", detail);
            (void)state_write_status(
                state, spec, "qualification-failed", detail);
            return 0;
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
        return 0;
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
            return 0;
        }
    }
    if (generate_fragmented_data(spec, mountpoint, records, &record_count, &directory_entries) != 0) {
        const char *const export_argv[] = {"zpool", "export", pool, NULL};
        (void)run_process(export_argv, NULL, 0);
        emit_status(spec->key, "formatted-unpopulated", "ZFS payload generation failed");
        (void)state_write_status(state, spec, "formatted-unpopulated", "ZFS payload generation failed");
        return 0;
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
        return 0;
    }
    {
        char detail[512];
        if (require_fragmentation_state(
                spec, partition, 1, detail, sizeof(detail)) != 0) {
            emit_status(spec->key, "qualification-failed", detail);
            (void)state_write_status(
                state, spec, "qualification-failed", detail);
            return 0;
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
        return 0;
    }
    printf("+ built-in raw C APFS fixture creator %s\n", partition);
    fflush(stdout);
    if (ldtm_format_apfs_volume(partition) != 0) {
        emit_status(spec->key, "format-failed",
                    "built-in C APFS creator failed");
        (void)state_write_status(
            state, spec, "format-failed",
            "built-in C APFS creator failed");
        return 0;
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
        return 0;
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
            return 0;
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

int ldtm_worker_prepare(const char *device, const char *confirmed_device) {
    char canonical[PATH_MAX];
    char confirmed[PATH_MAX];
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
    if (ldtm_build_sfdisk_script(script, sizeof(script)) != 0) return 2;
    {
        const char *const argv[] = {"sfdisk", "--wipe", "always", "--lock", canonical, NULL};
        if (run_process(argv, script, 0) != 0) return 2;
    }
    (void)settle_partitions(canonical);
    if (load_partition_map(canonical, &map) != 0) {
        fputs("New partition table did not settle correctly.\n", stderr);
        return 2;
    }
    work = mkdtemp(work_template);
    if (work == NULL) return 2;
    if (ensure_directory(LDTM_STATE_ROOT, 0755) != 0 ||
        state_path_for_device(canonical, state_path, sizeof(state_path)) != 0) goto cleanup;
    state = fopen(state_path, "w");
    if (state == NULL) goto cleanup;
    (void)chmod(state_path, 0644);
    if (fprintf(state, "schema\t1\ndevice\t%s\n", canonical) < 0 || fflush(state) != 0) goto cleanup;

    for (index = 0U; index < ldtm_spec_count(); ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        const char *partition = partition_for_label(&map, spec->label);
        printf("\n=== %s: %s ===\n", spec->key, partition != NULL ? partition : "missing partition");
        fflush(stdout);
        if (partition == NULL) {
            emit_status(spec->key, "failed", "partition label was not found after repartitioning");
            (void)state_write_status(state, spec, "failed", "partition label missing");
            continue;
        }
        if (spec->creator == LDTM_CREATOR_MANUAL) {
            emit_status(spec->key, "reserved", spec->note);
            (void)state_write_status(state, spec, "reserved", spec->note);
            continue;
        }
        if ((spec->creator == LDTM_CREATOR_AFFS || spec->creator == LDTM_CREATOR_PFS3)) {
            if (create_amiga_and_populate(spec, partition, state) != 0) goto cleanup;
        } else if (spec->creator == LDTM_CREATOR_APFS) {
            if (create_apfs_and_populate(spec, partition, state) != 0) goto cleanup;
        } else if (spec->creator == LDTM_CREATOR_UFS) {
            if (create_ufs_and_populate(spec, partition, work, state) != 0) goto cleanup;
        } else if (spec->creator == LDTM_CREATOR_ZFS) {
            if (create_zfs_and_populate(spec, partition, work, canonical, state) != 0) goto cleanup;
        } else {
            if (create_regular_and_populate(spec, partition, work, state) != 0) goto cleanup;
        }
    }
    result = 0;
    printf("\nTest disk preparation finished. State: %s\n", state_path);
    fflush(stdout);

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

static int load_verify_state(const char *state_path, LdtmVerifyFilesystem state[LDTM_SPEC_COUNT]) {
    FILE *stream = fopen(state_path, "r");
    char line[LDTM_LINE_MAX];
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
        if (strcmp(fields[0], "fs") == 0 && field_count >= 3U) {
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
    if (!infiltratr_path_join(directory_path, sizeof(directory_path), root, "fragmented-directory") ||
        directory_entry_count(directory_path) != expected->directory_entries) {
        emit_status(spec->key, "verify-failed", "directory-entry count changed");
        return -1;
    }
    emit_status(spec->key, "verified", "all retained file hashes, sizes and directory entries match");
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
        load_verify_state(state_path, expected) != 0) {
        fprintf(stderr, "No test-media state found for %s.\n", canonical);
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
