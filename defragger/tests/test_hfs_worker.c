// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define IMAGE_BYTES 16384

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                          __LINE__, #expr);                                   \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

static void write_all(int fd, const void *buffer, size_t length, off_t offset)
{
    const uint8_t *bytes = buffer;
    size_t done = 0U;
    while (done < length) {
        const ssize_t count = pwrite(fd, bytes + done, length - done,
                                     offset + (off_t)done);
        CHECK(count > 0);
        done += (size_t)count;
    }
}

static void write_be16(int fd, off_t offset, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xffU),
    };
    write_all(fd, bytes, sizeof(bytes), offset);
}

static void write_be32(int fd, off_t offset, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)(value >> 24),
        (uint8_t)((value >> 16) & 0xffU),
        (uint8_t)((value >> 8) & 0xffU),
        (uint8_t)(value & 0xffU),
    };
    write_all(fd, bytes, sizeof(bytes), offset);
}

static void make_image(int fd)
{
    static const uint8_t signature[2] = {0x42U, 0x44U};
    static const uint8_t bitmap[2] = {0xb1U, 0x01U};

    CHECK(ftruncate(fd, IMAGE_BYTES) == 0);
    write_all(fd, signature, sizeof(signature), 1024);
    write_be16(fd, 1024 + 14, 3U);
    write_be16(fd, 1024 + 18, 16U);
    write_be32(fd, 1024 + 20, 512U);
    write_be16(fd, 1024 + 28, 4U);
    write_be16(fd, 1024 + 34, 11U);
    write_be32(fd, 1024 + 84, 2U);
    write_be32(fd, 1024 + 88, 1U);
    write_all(fd, bitmap, sizeof(bitmap), 3 * 512);
}

static int run_worker(const char *worker, const char *mode, const char *image,
                      const char *option, const char *value,
                      char *output, size_t output_size)
{
    int pipefd[2];
    pid_t child;
    size_t used = 0U;
    int status = 0;

    CHECK(output_size > 0U);
    CHECK(pipe(pipefd) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(pipefd[1]);
        if (option != NULL)
            execl(worker, worker, mode, image, option, value, (char *)NULL);
        else
            execl(worker, worker, mode, image, (char *)NULL);
        _exit(127);
    }

    (void)close(pipefd[1]);
    while (used + 1U < output_size) {
        const ssize_t count = read(pipefd[0], output + used,
                                   output_size - used - 1U);
        if (count < 0)
            continue;
        if (count == 0)
            break;
        used += (size_t)count;
    }
    output[used] = '\0';
    (void)close(pipefd[0]);
    CHECK(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status))
        return 128;
    return WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
    char image[] = "/tmp/defragmenter-hfs-test-XXXXXX";
    char output[16384];
    static const uint8_t invalid_signature[2] = {0U, 0U};

    CHECK(argc == 2);
    const int fd = mkstemp(image);
    CHECK(fd >= 0);
    make_image(fd);

    CHECK(run_worker(argv[1], "identify", image, NULL, NULL,
                     output, sizeof(output)) == 0);
    CHECK(strcmp(output,
                 "{\"filesystem\":\"hfs\",\"block_size\":512,\"total_blocks\":16}\n") == 0);

    CHECK(run_worker(argv[1], "map", image, "--cells", "4",
                     output, sizeof(output)) == 0);
    CHECK(strstr(output,
                 "\"filesystem\":\"hfs\",\"map_accuracy\":\"exact\","
                 "\"unit_size\":512,\"total_units\":16,\"cell_count\":4") != NULL);
    CHECK(strstr(output,
                 "\"start\":0,\"end\":3,\"free\":1,\"used\":3,\"unknown\":0") != NULL);
    CHECK(strstr(output,
                 "\"start\":4,\"end\":7,\"free\":3,\"used\":1,\"unknown\":0") != NULL);
    CHECK(strstr(output,
                 "\"start\":8,\"end\":11,\"free\":4,\"used\":0,\"unknown\":0") != NULL);
    CHECK(strstr(output,
                 "\"start\":12,\"end\":15,\"free\":3,\"used\":1,\"unknown\":0") != NULL);
    CHECK(strstr(output,
                 "\"total_bytes\":8192,\"free_bytes\":5632,"
                 "\"used_bytes\":2560,\"unknown_bytes\":0") != NULL);
    CHECK(strstr(output, "\"header_free_blocks\":11") != NULL);

    write_all(fd, invalid_signature, sizeof(invalid_signature), 1024);
    CHECK(run_worker(argv[1], "identify", image, NULL, NULL,
                     output, sizeof(output)) != 0);

    CHECK(close(fd) == 0);
    CHECK(unlink(image) == 0);
    (void)puts("Classic HFS native worker tests passed");
    return 0;
}
