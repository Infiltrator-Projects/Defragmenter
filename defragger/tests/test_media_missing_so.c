// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A missing independent EXFAT/EXT provider must not permit Defragmenter
 * Test Media to erase existing signatures before a format operation fails.
 * This regression never opens a real block device or needs root privileges.
 */
#define _POSIX_C_SOURCE 200809L
#include <openssl/evp.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int digest_file(const char *path, unsigned char digest[32])
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context) { fclose(file); return -1; }
    int ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    unsigned char buffer[65536];
    while (ok) {
        size_t bytes = fread(buffer, 1, sizeof(buffer), file);
        if (bytes && EVP_DigestUpdate(context, buffer, bytes) != 1)
            ok = 0;
        if (bytes < sizeof(buffer)) {
            if (ferror(file)) ok = 0;
            break;
        }
    }
    unsigned size = 0;
    if (ok)
        ok = EVP_DigestFinal_ex(context, digest, &size) == 1 &&
             size == 32U;
    EVP_MD_CTX_free(context);
    if (fclose(file) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int refuse_missing_module(const char *binary, const char *format,
                                 const char *image, const char *empty_root)
{
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        if (setenv("IFS_TEST_MODULE_PARENT", empty_root, 1) != 0)
            _exit(127);
        int quiet = open("/dev/null", O_WRONLY);
        if (quiet < 0 || dup2(quiet, STDOUT_FILENO) < 0 ||
            dup2(quiet, STDERR_FILENO) < 0)
            _exit(127);
        close(quiet);
        execl(binary, binary, "create", format, image, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
           WEXITSTATUS(status) != 127 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s NATIVE_TEST_MEDIA_BINARY\n", argv[0]);
        return 2;
    }
    char work[] = "/tmp/defrag-missing-so-XXXXXX";
    if (!mkdtemp(work)) return 1;
    char root[sizeof(work) + 48U], directory[sizeof(work) + 64U];
    char image[sizeof(work) + 32U];
    int result = 1;
    if (snprintf(root, sizeof(root), "%s/no-modules", work) >=
        (int)sizeof(root) ||
        snprintf(directory, sizeof(directory), "%s/Filesystems", root) >=
        (int)sizeof(directory) ||
        snprintf(image, sizeof(image), "%s/media.img", work) >=
        (int)sizeof(image))
        goto done;
    if (mkdir(root, 0700) != 0 || mkdir(directory, 0700) != 0)
        goto done;
    int fd = open(image, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) goto done;
    unsigned char marker[4096];
    memset(marker, 0xA5, sizeof(marker));
    const off_t bytes = (off_t)(8U * 1024U * 1024U);
    int ok = ftruncate(fd, bytes) == 0 &&
             pwrite(fd, marker, sizeof(marker), 0) == (ssize_t)sizeof(marker) &&
             pwrite(fd, marker, sizeof(marker), bytes - (off_t)sizeof(marker)) ==
                 (ssize_t)sizeof(marker) &&
             fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok) goto done;
    unsigned char before[32], after[32];
    if (digest_file(image, before) != 0) goto done;
    const char *const formats[] = {"exfat", "ext2", "ext3", "ext4"};
    for (size_t n = 0U; n < sizeof(formats) / sizeof(formats[0]); ++n) {
        if (refuse_missing_module(argv[1], formats[n], image, root) != 0 ||
            digest_file(image, after) != 0 ||
            memcmp(before, after, sizeof(before)) != 0) {
            fprintf(stderr, "missing %s.so was not fail-closed and byte-preserving\n",
                    formats[n]);
            goto done;
        }
    }
    puts("Missing EXFAT/EXT providers: all rejected without changing media bytes");
    result = 0;
done:
    unlink(image);
    rmdir(directory);
    rmdir(root);
    rmdir(work);
    return result;
}
