// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include "test_media_raw.h"
#include "filesystem_module_host.h"
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
#include <linux/fs.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LDTM_HASH_HEX 65U
#define LDTM_LINE_MAX 4096U
#define LDTM_CAPTURE_MAX (8U * 1024U * 1024U)
#define LDTM_PROCESS_TIMEOUT_MS UINT64_C(600000)
#define LDTM_PROCESS_POLL_MS 100

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

static int require_fragmentation_state(const LdtmFilesystemSpec *spec,
                                       const char *partition,
                                       int expect_fragmented,
                                       char *detail, size_t detail_capacity) {
    if(!strcmp(spec->key,"bfs")) {
        int rc=ldtm_common_v2_verify_bfs_layout(spec,partition,expect_fragmented);
        if(!rc && expect_fragmented==2)rc=ldtm_common_v2_verify_growth(spec,partition);
        (void)snprintf(detail,detail_capacity,
            "BFS contiguous-file %s layout and corpus verification %s",
            expect_fragmented==1?"free-space fragmentation":expect_fragmented==2?"10% growth reserve":"compaction",
            rc?"failed":"passed");
        return rc;
    }
    unsigned fragmented=0u;
    if(ldtm_common_v2_verify_fragmentation(spec,partition,&fragmented)!=0) {
        (void)snprintf(detail,detail_capacity,"Common V2 corpus bytes or extent verification failed");
        return -1;
    }
    const unsigned expected=expect_fragmented==1?LDTM_TARGET_FILE_COUNT:0u;
    if(fragmented!=expected) {
        (void)snprintf(detail,detail_capacity,
            "Common V2 extent verification requires %u fragmented corpus files; found %u",
            expected,fragmented);
        return -1;
    }
    if(expect_fragmented==2) {
        if(ldtm_common_v2_verify_growth(spec,partition)!=0) {
            (void)snprintf(detail,detail_capacity,
                "Canonical Common V2 allocation map did not prove every 10%% growth reserve");
            return -1;
        }
        (void)snprintf(detail,detail_capacity,
            "Common V2 corpus bytes, contiguous files and all eight 10%% growth reserves verified");
        return 0;
    }
    (void)snprintf(detail,detail_capacity,
        "Common V2 corpus bytes verified; %u large files fragmented",fragmented);
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

/*
 * The MMC CID is the hardware identity of a particular SD/eMMC medium.
 * Udev enriches serial, WWN and bus descriptions asynchronously, especially
 * while a desktop unmounts user-media partitions. Those descriptions are NOT
 * stable confirmation material. Bind card CID + kernel device number + actual
 * capacity instead; refuse anonymous/all-zero CID cards rather than falling
 * back to model/capacity or silently relaxing the destructive confirmation.
 *
 * This pure builder is also directly exercised by the Test Media unit suite.
 */
int ldtm_mmc_identity_material(
    uint64_t size_bytes, unsigned int major_number, unsigned int minor_number,
    const char *cid, char *material, size_t capacity)
{
    char normalised_cid[33];
    int nonzero = 0;
    if (size_bytes == 0U || cid == NULL || material == NULL ||
        capacity == 0U || strlen(cid) != 32U)
        return -1;
    for (size_t index = 0U; index < 32U; ++index) {
        const unsigned char value = (unsigned char)cid[index];
        if (!isxdigit(value))
            return -1;
        normalised_cid[index] = (char)tolower(value);
        if (normalised_cid[index] != '0')
            nonzero = 1;
    }
    if (!nonzero)
        return -1;
    normalised_cid[32] = '\0';
    const int length = snprintf(
        material, capacity,
        "mmc-cid-v1\nsize=%llu\nrdev=%u:%u\ncid=%s\n",
        (unsigned long long)size_bytes, major_number, minor_number,
        normalised_cid);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
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

    const char *base = infiltratr_path_basename(canonical);
    if (base != NULL && strncmp(base, "mmcblk", 6U) == 0) {
        /*
         * Physical MMC card identity must not depend on opportunistically
         * populated udev properties. Read the kernel card CID directly.
         * Its absence/invalidity is a hard refusal, never an anonymous
         * model/size fallback. Kernel device number binds this invocation
         * to the selected block target even if a different card is inserted.
         */
        struct stat status;
        char cid_path[PATH_MAX];
        char cid[96] = "";
        if (stat(canonical, &status) != 0 || !S_ISBLK(status.st_mode))
            return -1;
        const int path_length = snprintf(
            cid_path, sizeof(cid_path),
            "/sys/dev/block/%u:%u/device/cid",
            major(status.st_rdev), minor(status.st_rdev));
        if (path_length <= 0 || (size_t)path_length >= sizeof(cid_path) ||
            !infiltratr_read_text_file(cid_path, cid, sizeof(cid)))
            return -1;
        infiltratr_trim_line_end(cid);
        if (ldtm_mmc_identity_material(
                info.size_bytes, major(status.st_rdev),
                minor(status.st_rdev), cid, material,
                sizeof(material)) != 0)
            return -1;
    } else {
        /*
         * Other device classes retain their original trusted physical
         * serial/WWN identity contract; never approve an anonymous disk
         * from a model/capacity match.
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
    }

    context = EVP_MD_CTX_new();
    if (context == NULL ||
        EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(context, material, strlen(material)) != 1 ||
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

/* Resolve the exact selected disk and its descendants to kernel device IDs.
 * Never use a previously observed mountpoint as authority to unmount: the
 * desktop may already have removed it, or mounted another disk there. */
typedef struct {
    unsigned int major_number;
    unsigned int minor_number;
} LdtmMountedDevice;

#define LDTM_MAX_UNMOUNT_DEVICES 256U
#define LDTM_MAX_UNMOUNT_ATTEMPTS 256U

static bool ldtm_matches_device_number(
    unsigned int major_number, unsigned int minor_number,
    const LdtmMountedDevice *devices, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        if (devices[index].major_number == major_number &&
            devices[index].minor_number == minor_number)
            return true;
    }
    return false;
}

static int ldtm_mountinfo_target(
    const char *line, const LdtmMountedDevice *devices, size_t count,
    char target[PATH_MAX])
{
    const char *cursor = line;
    const char *fields[5];
    size_t lengths[5];
    unsigned int major_number = 0U, minor_number = 0U;
    char number[64];
    char extra;

    if (line == NULL || devices == NULL || target == NULL)
        return -1;
    target[0] = '\0';
    for (unsigned int field = 0U; field < 5U; ++field) {
        while (*cursor == ' ') ++cursor;
        if (*cursor == '\0' || *cursor == '\n') return -1;
        fields[field] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\n')
            ++cursor;
        lengths[field] = (size_t)(cursor - fields[field]);
    }
    if (lengths[2] == 0U || lengths[2] >= sizeof(number))
        return -1;
    memcpy(number, fields[2], lengths[2]);
    number[lengths[2]] = '\0';
    if (sscanf(number, "%u:%u%c", &major_number, &minor_number, &extra) != 2)
        return -1;
    if (!ldtm_matches_device_number(major_number, minor_number, devices, count)) {
        /* Btrfs and other filesystems can expose a virtual superblock
         * major:minor. Resolve their mountinfo SOURCE block device too. */
        const char *separator = strstr(line, " - ");
        char source[PATH_MAX];
        struct stat source_status;
        if (separator == NULL ||
            sscanf(separator + 3, "%*s %4095s", source) != 1 ||
            stat(source, &source_status) != 0 ||
            !S_ISBLK(source_status.st_mode) ||
            !ldtm_matches_device_number(
                major(source_status.st_rdev), minor(source_status.st_rdev),
                devices, count))
            return 0;
    }

    /* mountinfo escapes spaces, tabs, newlines and backslashes as octal.
     * Decode only an exact kernel mountpoint field, never shell-evaluate it. */
    size_t used = 0U;
    for (size_t index = 0U; index < lengths[4]; ++index) {
        unsigned char value = (unsigned char)fields[4][index];
        if (value == '\\') {
            if (index + 3U >= lengths[4])
                return -1;
            unsigned int decoded = 0U;
            for (unsigned int digit = 1U; digit <= 3U; ++digit) {
                const char byte = fields[4][index + digit];
                if (byte < '0' || byte > '7')
                    return -1;
                decoded = decoded * 8U + (unsigned int)(byte - '0');
            }
            if (decoded == 0U || decoded > 255U)
                return -1;
            value = (unsigned char)decoded;
            index += 3U;
        }
        if (used + 1U >= PATH_MAX)
            return -1;
        target[used++] = (char)value;
    }
    if (used == 0U)
        return -1;
    target[used] = '\0';
    return 1;
}

static int ldtm_selected_device_numbers(
    const char *device, LdtmMountedDevice *devices, size_t *count)
{
    const char *const argv[] = {
        "lsblk", "-n", "-r", "-o", "MAJ:MIN", "--", device, NULL
    };
    char *output = NULL, *saveptr = NULL;
    if (device == NULL || devices == NULL || count == NULL ||
        capture_process(argv, &output) != 0)
        return -1;
    *count = 0U;
    char *line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        unsigned int major_number = 0U, minor_number = 0U;
        char extra;
        infiltratr_trim(line);
        if (sscanf(line, "%u:%u%c", &major_number, &minor_number, &extra) != 2 ||
            *count >= LDTM_MAX_UNMOUNT_DEVICES) {
            free(output);
            return -1;
        }
        if (!ldtm_matches_device_number(
                major_number, minor_number, devices, *count)) {
            devices[*count].major_number = major_number;
            devices[*count].minor_number = minor_number;
            ++*count;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(output);
    return *count > 0U ? 0 : -1;
}

static int unmount_descendants(const char *device)
{
    LdtmMountedDevice devices[LDTM_MAX_UNMOUNT_DEVICES];
    size_t count = 0U;
    if (ldtm_selected_device_numbers(device, devices, &count) != 0)
        return -1;

    /* Re-read live mountinfo after each unmount. An old lsblk MOUNTPOINT
     * snapshot can contain directories no longer mounted (the reported
     * LD_EXT3/LD_EXT4 regression), or now owned by another device. */
    unsigned int failures = 0U;
    for (unsigned int attempt = 0U;
         attempt < LDTM_MAX_UNMOUNT_ATTEMPTS; ++attempt) {
        FILE *mounts = fopen("/proc/self/mountinfo", "r");
        char *line = NULL;
        size_t capacity = 0U;
        char target[PATH_MAX] = "";
        size_t longest = 0U;
        unsigned int target_major = 0U, target_minor = 0U;
        int parse_failure = 0;

        if (mounts == NULL)
            return -1;
        while (getline(&line, &capacity, mounts) >= 0) {
            char current[PATH_MAX];
            const int matches =
                ldtm_mountinfo_target(line, devices, count, current);
            if (matches < 0) {
                parse_failure = 1;
                break;
            }
            if (matches > 0 && strlen(current) > longest) {
                /* Remember the selected live mount's superblock identity.
                 * It can be virtual for Btrfs, not the partition MAJ:MIN. */
                if (sscanf(line, "%*s %*s %u:%u",
                           &target_major, &target_minor) != 2) {
                    parse_failure = 1;
                    break;
                }
                longest = strlen(current);
                (void)snprintf(target, sizeof(target), "%s", current);
            }
        }
        if (ferror(mounts))
            parse_failure = 1;
        free(line);
        if (fclose(mounts) != 0 || parse_failure)
            return -1;

        if (longest == 0U)
            return ld_path_is_mounted(device) ? -1 : 0;

        /* stat() identifies the TOP mount at this path. Refuse to unmount
         * any unrelated filesystem covering the selected disk's mount. */
        struct stat status;
        if (stat(target, &status) != 0 ||
            major(status.st_dev) != target_major ||
            minor(status.st_dev) != target_minor) {
            if (++failures >= 3U)
                return -1;
            continue;
        }
        /* Unmount through the kernel directly. Do not depend on the host's
         * util-linux umount binary, option parsing or PATH. No lazy detach:
         * the selected filesystem must actually be released before GPT
         * mutation. Retain the fail-closed post-unmount topology check. */
        printf("+ unmount selected filesystem %s\n", target);
        fflush(stdout);
        if (umount2(target, 0) != 0) {
            const int failure = errno;
            /* A desktop automount race may have already removed this exact
             * mount. Re-read the live mount table rather than trusting a
             * transient ENOENT/EINVAL. Persistent EBUSY or ownership changes
             * remain fatal; never detach an unrelated filesystem. */
            fprintf(stderr, "Unmount of %s: %s\n",
                    target, strerror(failure));
            if (++failures >= 3U)
                return -1;
        } else {
            failures = 0U;
        }
    }
    return -1;
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

static void digest_to_hex(const unsigned char *digest, unsigned int length, char output[LDTM_HASH_HEX]) {
    unsigned int index;
    static const char hex[] = "0123456789abcdef";
    for (index = 0U; index < length && index < 32U; ++index) {
        output[index * 2U] = hex[(digest[index] >> 4) & 0x0fU];
        output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    output[64] = '\0';
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

/* One qualification implementation is shared by disposable preflight and
 * actual partition population. The preflight must not emit a populated state
 * for the physical medium: that state belongs only to successful real writes. */
static int qualify_corpus(const LdtmFilesystemSpec *spec, const char *partition,
                          char *detail, size_t capacity)
{
    if (!ldtm_spec_creator_available(spec, detail, capacity) ||
        ldtm_raw_fixture(spec, partition, true, detail, capacity) != 0)
        return -1;
    if (spec->creator == LDTM_CREATOR_SWAP)
        return ldtm_raw_fixture(spec, partition, false, detail, capacity);
    if(!strcmp(spec->key,"bfs"))return require_fragmentation_state(spec,partition,1,detail,capacity);
    unsigned fragmented = 0U;
    /* The independent read-only extent pass also verifies every corpus byte,
     * so another byte-only reopen between the two passes is unnecessary. */
    if (ldtm_common_v2_verify_fragmentation(spec, partition, &fragmented) != 0 ||
        fragmented != LDTM_TARGET_FILE_COUNT) {
        (void)snprintf(detail, capacity,
            "Common V2 corpus extent verification requires all %u large files fragmented; proved %u",
            LDTM_TARGET_FILE_COUNT, fragmented);
        return -1;
    }
    return 0;
}

static int create_raw_and_populate(const LdtmFilesystemSpec *spec, const char *partition,
                                   FILE *state)
{
    char detail[512] = {0};
    if (qualify_corpus(spec, partition, detail, sizeof(detail)) != 0) {
        emit_status(spec->key, "qualification-failed", detail);
        return state_write_status(state, spec, "qualification-failed", detail) == 0 ? 1 : -1;
    }
    const char *message =
        spec->creator == LDTM_CREATOR_SWAP
            ? "Common V2 SWAP header verified; file fragmentation does not apply"
            : !strcmp(spec->key,"bfs")
              ? "Common V2 BFS corpus verified; provider maps proved eight genuine free-space gaps"
              : "Common V2 corpus fully verified; provider extent maps proved all eight large files fragmented";
    emit_status(spec->key, "populated", message);
    return state_write_status(state, spec, "populated", message);
}

/* Actual provider qualification before the destructive boundary. Use one
 * private, sparse, exact-size regular image at a time. Its filesystem bytes
 * are written exclusively by the provider, never by the application.
 * Failure (including insufficient scratch space) leaves the selected disk
 * untouched and its existing mounts intact. */
static int run_production_defrag(const LdtmFilesystemSpec *spec,
    const char *partition, const char *operation);

static int preflight_image(const LdtmFilesystemSpec *spec,
                           const char *operation, char *detail, size_t capacity)
{
    char directory[] = "/tmp/defragmenter-provider-check.XXXXXX";
    if (!mkdtemp(directory)) {
        (void)snprintf(detail, capacity, "Cannot create private provider preflight directory: %s",
                       strerror(errno));
        return -1;
    }
    char image[sizeof(directory) + 32U];
    int result = -1;
    int length = snprintf(image, sizeof(image), "%s/media.img", directory);
    if (length < 0 || (size_t)length >= sizeof(image)) {
        (void)snprintf(detail, capacity, "Cannot construct provider preflight image path");
        goto cleanup;
    }
    int fd = open(image, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        (void)snprintf(detail, capacity, "Cannot create provider preflight image: %s",
                       strerror(errno));
        goto cleanup;
    }
    int sized = ftruncate(fd, (off_t)((uint64_t)spec->size_mib * LDTM_MIB)) == 0;
    int closed = close(fd) == 0;
    if (!sized || !closed) {
        (void)snprintf(detail, capacity, "Cannot size or close provider preflight image");
    } else {
        result = qualify_corpus(spec, image, detail, capacity);
        /* A successful corpus is not evidence that the mutation engine works.
         * Invoke the same strict host used on the actual disk, then reopen
         * read-only and verify both allocation and every payload byte. */
        if (result == 0 && operation != NULL) {
            if (run_production_defrag(spec, image, operation) != 0) {
                (void)snprintf(detail, capacity,
                    "Disposable Common V2 %s failed", operation);
                result = -1;
            } else {
                result = require_fragmentation_state(spec, image, !strcmp(operation,"growth-defrag")?2:0, detail, capacity);
            }
        }
    }
    if (unlink(image) != 0) {
        (void)snprintf(detail, capacity, "Cannot remove provider preflight image: %s",
                       strerror(errno));
        result = -1;
    }
cleanup:
    if (rmdir(directory) != 0) {
        (void)snprintf(detail, capacity, "Cannot remove provider preflight directory: %s",
                       strerror(errno));
        result = -1;
    }
    return result;
}

/* Keep corpus-only qualification available for its dedicated regression.
 * Physical preparation additionally requires both mutation paths on separate
 * freshly formatted, independently fragmented disposable images. SWAP has
 * no files and is qualified by its applicable corpus/header operations. */
static int preflight_corpus(const LdtmFilesystemSpec *spec,
                            char *detail, size_t capacity)
{
    return preflight_image(spec, NULL, detail, capacity);
}

static int preflight_mutations(const LdtmFilesystemSpec *spec,
                               char *detail, size_t capacity)
{
    if (spec->creator == LDTM_CREATOR_SWAP)
        return 0;
    if (preflight_image(spec, "defrag", detail, capacity) != 0)
        return -1;
    return preflight_image(spec, "growth-defrag", detail, capacity);
}

/* Crash the child at a successfully completed durable barrier, never
 * after a failed flush and never via the provider's error-unwind path.
 * A fresh process then remounts the same image and exercises RECOVER.
 * All callbacks except flush are exactly the production host services.
 * The test does not parse a filesystem journal or modify raw metadata. */
typedef struct {
    const LdtmFilesystemSpec *spec;
    const char *path;
    unsigned boundary;
    uint32_t policy;
    uint64_t repaired;
} RecoveryFixture;
static int (*recovery_real_flush)(void *);
static unsigned recovery_boundary,recovery_flushes;
static int recovery_fault_flush(void *context)
{
    int rc=recovery_real_flush(context);
    if(!rc && ++recovery_flushes==recovery_boundary)_exit(86);
    return rc;
}
static int recovery_fixture_call(const IfsDefragHostV2 *host,void *symbol,void *opaque)
{
    RecoveryFixture *f=opaque;
    const char *id=!strcmp(f->spec->key,"ufs")?"ufs2":f->spec->key;
    IfsFsModuleApiEntryV2 entry=NULL;
    _Static_assert(sizeof(entry)==sizeof(symbol),"Common V2 entry width");
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry?entry():NULL;
    if(!api || !ifs_fs_api_v2_valid(api,id) ||
       !(api->runtime_profiles&IFS_FS_PROFILE_HOSTED))return -1;
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsDefragOpsV2 *defrag=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    if(!core || !core->mount || !core->unmount || !defrag ||
       (defrag->header.capabilities&IFS_FS_DEFRAG_ALL)!=IFS_FS_DEFRAG_ALL ||
       !defrag->plan || !defrag->execute || !defrag->recover ||
       !defrag->verify || !defrag->release_plan)return -1;
    IfsFilesystemTargetV2 binding={.services=host};
    if(host->open_target(host->context,f->path,1,NULL,0u,&binding.target)!=0)
        return -1;
    IfsFsHostRuntimeV2 runtime={0};
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *session=NULL,*plan=NULL;
    int rc=-1;
    if(ifs_filesystem_target_runtime_v2(&binding,1,0u,&runtime)!=0 ||
       core->mount(&runtime,&session,&info)!=IFS_FS_OK || !session)
        goto done;
    if(f->boundary) {
        /* Install the wrapper BEFORE mount only on the next fresh
         * session: providers retain a private copy of the runtime. */
        if(core->unmount(session)!=IFS_FS_OK){session=NULL;goto done;}
        session=NULL;
        recovery_real_flush=runtime.flush;
        if(!recovery_real_flush)goto done;
        recovery_boundary=UINT_MAX;recovery_flushes=0u;
        runtime.flush=recovery_fault_flush;
        info.structure_bytes=sizeof(info);
        if(core->mount(&runtime,&session,&info)!=IFS_FS_OK || !session)
            goto done;
        if(defrag->plan(session,"/",f->policy,&plan)!=IFS_FS_OK || !plan)
            goto done;
        recovery_flushes=0u;recovery_boundary=f->boundary;
        uint64_t moved=0u;
        /* A return here means the chosen durable boundary was not hit.
         * It must never be mistaken for an interrupted relocation. */
        (void)defrag->execute(session,plan,&moved);
        recovery_boundary=UINT_MAX;
    }else if(defrag->recover(session,0u,&f->repaired)==IFS_FS_OK)
        rc=0;
done:
    recovery_boundary=UINT_MAX;
    if(plan)defrag->release_plan(session,plan);
    if(session && core->unmount(session)!=IFS_FS_OK)rc=-1;
    if(!rc && host->sync_target(host->context,&binding.target)!=0)rc=-1;
    host->close_target(host->context,&binding.target);
    return rc;
}
static int recovery_fixture_invoke(RecoveryFixture *f)
{
    const char *id=!strcmp(f->spec->key,"ufs")?"ufs2":f->spec->key;
    char module[64]={0};
    if(ifs_filesystem_module_find_readonly_v2(id,module,sizeof(module))!=0)return -1;
    return ifs_filesystem_module_call(id,module,IFS_FS_COMMON_EXPORT_V2,
                                     recovery_fixture_call,f);
}
/* The installed worker and CI use this identical, disposable recovery
 * qualification. Failure must occur before any physical device is unmounted
 * or modified. A provider that cannot replay a durable interrupted mutation
 * has not qualified for either destructive operation. */
static int preflight_recovery(const LdtmFilesystemSpec *spec,
                               char *detail,size_t capacity)
{
    if(spec->creator==LDTM_CREATOR_SWAP)return 0;
    for(uint32_t policy=0u;policy<=10u;policy+=10u) {
    unsigned proven_replay=0u;
    /* Each crash starts from a separately formatted and verified
     * fragmented corpus. No previous recovery can mask a later fault.
     * Four successive barriers cover data-before-metadata and redo
     * publication/application for the native transactional writers. */
    for(unsigned boundary=1u;boundary<=4u;++boundary) {
        char dir[]="/tmp/defragmenter-recovery.XXXXXX",path[PATH_MAX],failure[512]={0};
        if(!mkdtemp(dir)) {
            (void)snprintf(detail,capacity,"Cannot create private recovery image directory");
            return -1;
        }
        int length=snprintf(path,sizeof(path),"%s/media.img",dir);
        int ok=length>0 && (size_t)length<sizeof(path);
        int fd=ok?open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600):-1;
        if(fd<0)ok=0;
        else {
            if(ftruncate(fd,(off_t)((uint64_t)spec->size_mib*LDTM_MIB))!=0)ok=0;
            if(close(fd)!=0)ok=0;
        }
        if(ok && qualify_corpus(spec,path,failure,sizeof(failure))!=0)ok=0;
        if(ok) {
            RecoveryFixture f={.spec=spec,.path=path,.boundary=boundary,.policy=policy};
            fflush(NULL);
            uint64_t started_ms=0u;
            pid_t pid=monotonic_millis(&started_ms)==0?fork():-1;
            if(pid==0){
                if(setpgid(0,0)!=0)_exit(87);
                (void)recovery_fixture_invoke(&f);_exit(87);
            }
            if(pid<0)ok=0;
            else {
                (void)setpgid(pid,pid);
                int status=0;
                if(wait_child_until(pid,&status,started_ms)!=0 ||
                   !WIFEXITED(status) || WEXITSTATUS(status)!=86)
                    ok=0;
            }
            if(ok) {
                f.boundary=0u;
                if(recovery_fixture_invoke(&f)!=0 ||
                   ldtm_raw_fixture(spec,path,false,failure,sizeof(failure))!=0)ok=0;
                else {
                    if(f.repaired)++proven_replay;
                    printf("%s: crash after durable barrier %u; recovered %llu; all payloads verified\n",
                           spec->key,boundary,(unsigned long long)f.repaired);
                }
            }
        }
        if(fd>=0 && unlink(path)!=0)ok=0;
        if(rmdir(dir)!=0)ok=0;
        if(!ok) {
            (void)snprintf(detail,capacity,
                "%s: durable recovery qualification failed for policy %u at barrier %u: %s",
                spec->key,policy,boundary,failure);
            return -1;
        }
    }
    if(!proven_replay) {
        (void)snprintf(detail,capacity,"No durable recovery replay was demonstrated");
        return -1;
    }
    printf("%s: interrupted native relocation policy %u and independent replay qualification passed\n",
           spec->key,policy);
    }
    (void)snprintf(detail,capacity,"Both Common V2 recovery policies passed");
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
    /* The complete Test Media layout includes the fixed-block OFS corpus.
     * Check this before unmounting or writing the new partition table. */
    LdDevice target;
    if (ld_device_try_open(canonical, false, &target) != 0) {
        perror("Could not open the confirmed Test Media disk for geometry checks");
        return 2;
    }
    int logical_sector = 0;
    int geometry = ioctl(target.fd, BLKSSZGET, &logical_sector);
    ld_device_close(&target);
    if (geometry != 0 || logical_sector != (int)LDTM_MEDIA_SECTOR_BYTES) {
        fprintf(stderr, "The OFS Test Media corpus requires %u-byte logical sectors.\n",
                LDTM_MEDIA_SECTOR_BYTES);
        return 2;
    }
    printf("Safety check: %s\n", safety);
    fflush(stdout);
    for (size_t n = 0U; n < ldtm_spec_count(); ++n) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[n];
        printf("Preflight: qualifying %s on a disposable image before touching the selected disk\n",
               spec->key);
        fflush(stdout);
        if (preflight_corpus(spec, safety, sizeof(safety)) != 0 ||
            preflight_mutations(spec, safety, sizeof(safety)) != 0 ||
            preflight_recovery(spec, safety, sizeof(safety)) != 0) {
            emit_status(spec->key, "qualification-failed", safety);
            fprintf(stderr, "Provider preflight failed; selected disk has not been repartitioned or unmounted.\n");
            return 2;
        }
    }
    /* A long-running preflight cannot authorize a newly attached disk. */
    if (ldtm_device_fingerprint(canonical, current_fingerprint) != 0 ||
        strcmp(current_fingerprint, confirmed_fingerprint) != 0 ||
        ldtm_device_safety_check(canonical, 0, safety, sizeof(safety)) != 0) {
        fputs("Selected disk changed or became unsafe during provider preflight.\n", stderr);
        return 2;
    }
    if (unmount_descendants(canonical) != 0) {
        fputs("Unable to unmount all descendants.\n", stderr);
        return 2;
    }
    if (ldtm_device_fingerprint(canonical, current_fingerprint) != 0) {
        fputs("Unable to re-read the physical disk identity after unmount; refusing repartitioning.\n",
              stderr);
        return 2;
    }
    if (strcmp(current_fingerprint, confirmed_fingerprint) != 0) {
        fprintf(stderr,
                "The physical disk identity changed before repartitioning "
                "(confirmed fingerprint %.16s, current %.16s); refusing to continue.\n",
                confirmed_fingerprint, current_fingerprint);
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
        const char *const argv[] = {"sfdisk", "--wipe", "never", "--wipe-partitions", "never", "--lock", canonical, NULL};
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
            /* The desktop automounter can remount a previous filesystem as
             * soon as sfdisk/udev has settled, even after the initial safety
             * unmount. Never run a module formatter over a mounted
             * partition. Release only mounts on this already-confirmed,
             * sacrificial test disk; refuse if an open consumer prevents it.
             * The module's O_EXCL open remains the final race-safe guard. */
            if (ld_path_is_mounted(partition)) {
                printf("Desktop automount detected on %s; unmounting selected test disk descendants before formatting.\n",
                       partition);
                fflush(stdout);
                if (unmount_descendants(canonical) != 0 ||
                    ld_path_is_mounted(partition)) {
                    emit_status(spec->key, "format-failed",
                                "partition was mounted or busy after partition-table settlement");
                    (void)state_write_status(state, spec, "format-failed",
                                             "partition mounted or busy; no formatting attempted");
                    ++failures;
                    continue;
                }
            }
            if (spec->creator == LDTM_CREATOR_MANUAL) {
                emit_status(spec->key, "qualification-failed", spec->note);
                (void)state_write_status(
                    state, spec, "qualification-failed", spec->note);
                failures++;
                continue;
            }
            filesystem_result = create_raw_and_populate(spec, partition, state);
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
    size_t filesystems = 0U;
    bool seen[LDTM_SPEC_COUNT] = {false};
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
            if (index >= LDTM_SPEC_COUNT || seen[index] ||
                strlen(fields[2]) >= sizeof(state[index].build_status) ||
                strlen(fields[3]) >= sizeof(state[index].build_detail))
                return -1;
            if (strcmp(fields[2], "populated") != 0 && strcmp(fields[2], "failed") != 0 &&
                strcmp(fields[2], "format-failed") != 0 &&
                strcmp(fields[2], "formatted-unpopulated") != 0 &&
                strcmp(fields[2], "qualification-failed") != 0 && strcmp(fields[2], "skipped") != 0)
                return -1;
            seen[index] = true;
            ++filesystems;
            state[index].populated = strcmp(fields[2], "populated") == 0;
            memcpy(state[index].build_status, fields[2], strlen(fields[2]) + 1U);
            memcpy(state[index].build_detail, fields[3], strlen(fields[3]) + 1U);
        } else
            return -1;
    }
    return !ferror(stream) && headers == 15U && filesystems == LDTM_SPEC_COUNT
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

        if (spec->creator == LDTM_CREATOR_SWAP) {
            char detail[512] = {0};
            if (ldtm_raw_fixture(spec, partition, false, detail, sizeof(detail)) != 0) {
                emit_status(spec->key, "verify-failed", detail);
                failures++;
            } else {
                emit_status(
                    spec->key, "verified",
                    "SWAP header verified by its Common V2 provider; no file allocation map applies");
            }
            continue;
        }

        {
            char detail[512];
            if (require_fragmentation_state(spec, partition,
                                            !strcmp(spec->key,"bfs")?2:0, detail,
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


/* Use exactly the same trusted Common V2 host as the operation engine.
 * Qualification never launches a replacement filesystem worker. */
static int run_production_defrag(const LdtmFilesystemSpec *spec,
    const char *partition, const char *operation)
{
    const char *format=!strcmp(spec->key,"ufs")?"ufs2":spec->key;
    char module[64]={0};
    if(ifs_filesystem_module_find_v2(format,module,sizeof(module))!=0)
        return -1;
    char *argv[]={(char *)"defragmenter-test-media",(char *)operation,
        (char *)partition,(char *)"--write",(char *)"--confirm",
        (char *)partition,NULL};
    return ifs_filesystem_module_run(format,module,6,argv);
}

int ldtm_worker_qualify(const char *device)
{
    char canonical[PATH_MAX];
    char safety[512];
    char state_path[PATH_MAX];
    char fingerprint[LDTM_HASH_HEX];
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
    /* Do not mutate a previously prepared disk until the currently installed
     * providers have passed both destructive policies on disposable media.
     * The provider package may have changed since Build Test Disk. */
    for (size_t index = 0U; index < LDTM_SPEC_COUNT; ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        if (!expected[index].populated ||
            preflight_corpus(spec, safety, sizeof(safety)) != 0 ||
            preflight_mutations(spec, safety, sizeof(safety)) != 0 ||
            preflight_recovery(spec, safety, sizeof(safety)) != 0) {
            fprintf(stderr, "Current disposable qualification failed for %s; physical media untouched.\n",
                    spec->key);
            return 2;
        }
    }
    char current_fingerprint[LDTM_HASH_HEX];
    if (ldtm_device_fingerprint(canonical, current_fingerprint) != 0 ||
        strcmp(current_fingerprint, fingerprint) != 0 ||
        ldtm_device_safety_check(canonical, 0, safety, sizeof(safety)) != 0) {
        fputs("Test disk changed or became unsafe during qualification.\n", stderr);
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

        if (spec->creator == LDTM_CREATOR_SWAP) {
            /* SWAP has no file allocation or file relocation operations.
             * Verify its real provider-owned header without claiming either. */
            if (ldtm_raw_fixture(spec, partition, false, detail, sizeof(detail)) != 0) {
                emit_status(spec->key, "qualification-failed", detail);
                ++failures;
            } else {
                emit_status(spec->key, "qualified",
                    "Common V2 SWAP header verified; file mutation does not apply");
            }
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

        // Never discard an external journal left by an old implementation.
        // It cannot be safely translated into provider-owned V2 recovery.
        if (access(journal, F_OK) == 0 || errno != ENOENT) {
            emit_status(spec->key,"qualification-failed",
                "Unresolved legacy journal; no Common V2 mutation attempted");
            ++failures;
            continue;
        }

        printf("+ production Defragment\n");
        fflush(stdout);
        if (require_fragmentation_state(spec, partition, 1, detail, sizeof(detail)) != 0 ||
            run_production_defrag(spec, partition, "defrag") != 0 ||
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

        /* Growth Defrag must relocate a fresh fragmented corpus, not
         * merely accept the packed result of ordinary Defragment. The
         * protected Test Media partition is explicitly disposable. Rebuild
         * it only with the same native provider and prove fragmentation. */
        detail[0] = '\0';
        if (qualify_corpus(spec, partition, detail, sizeof(detail)) != 0) {
            emit_status(spec->key, "qualification-failed", detail);
            ++failures;
            continue;
        }
        detail[0] = '\0';
        printf("+ production Growth Defrag (10%% reserve)\n");
        fflush(stdout);
        if (run_production_defrag(spec, partition, "growth-defrag") != 0 ||
            require_fragmentation_state(
                spec, partition, 2, detail, sizeof(detail)) != 0) {
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
            "Common V2 Defragment and Growth Defrag verified by provider; corpus bytes intact and all eight large files contiguous");
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
