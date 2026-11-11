// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * SWAPSPACE2 is a page store, not a relocatable file allocation tree.
 * Qualify the real independent SWAP.so formatter and probe, then require the
 * Defragmenter allocation mapper to reject the absent Common V2 DEFRAG group
 * without changing one byte of the media. This does not claim map support.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(const char *binary, char *const args[],
               char *output, size_t capacity)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);
        execv(binary, args);
        _exit(127);
    }
    close(pipefd[1]);
    size_t used = 0U;
    char chunk[2048];
    int read_error = 0;
    for (;;) {
        ssize_t count = read(pipefd[0], chunk, sizeof(chunk));
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            read_error = 1;
            break;
        }
        if (output && capacity > used + 1U) {
            size_t available = capacity - used - 1U;
            size_t copied = (size_t)count < available ?
                            (size_t)count : available;
            memcpy(output + used, chunk, copied);
            used += copied;
        }
    }
    close(pipefd[0]);
    if (output && capacity != 0U)
        output[used] = '\0';
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (read_error || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int media_sha256(const char *path, unsigned char digest[32])
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(file);
        return -1;
    }
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    unsigned char buffer[65536];
    while (ok) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count && EVP_DigestUpdate(ctx, buffer, count) != 1)
            ok = 0;
        if (count < sizeof(buffer)) {
            if (ferror(file))
                ok = 0;
            break;
        }
    }
    unsigned count = 0U;
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, digest, &count) == 1 &&
             count == 32U;
    EVP_MD_CTX_free(ctx);
    if (fclose(file) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <native-fixture> <mapper>\n", argv[0]);
        return 2;
    }

    char work[] = "/tmp/defragmenter-swap-v2-XXXXXX";
    if (!mkdtemp(work)) {
        perror("mkdtemp");
        return 1;
    }
    char image[sizeof(work) + 32U];
    if (snprintf(image, sizeof(image), "%s/fixture.img", work) >=
        (int)sizeof(image)) {
        rmdir(work);
        return 1;
    }

    char log[8192] = {0};
    unsigned char before[32] = {0}, after[32] = {0};
    int failed = 1;
    char *create[] = {argv[1], "create", "swap", image, NULL};
    char *verify[] = {argv[1], "verify", "swap", image, NULL};
    char *map[] = {argv[2], image, "--fstype", "swap",
                   "--cells", "8", NULL};

    if (run(argv[1], create, log, sizeof(log)) != 0) {
        fprintf(stderr, "independent SWAP.so format failed: %s\n", log);
        goto done;
    }
    if (run(argv[1], verify, log, sizeof(log)) != 0) {
        fprintf(stderr, "independent SWAP.so probe failed: %s\n", log);
        goto done;
    }
    if (media_sha256(image, before) != 0) {
        fprintf(stderr, "could not hash SWAP fixture\n");
        goto done;
    }
    int result = run(argv[2], map, log, sizeof(log));
    if (result != 2 ||
        !strstr(log, "allocation mapping is unavailable for this filesystem")) {
        fprintf(stderr, "SWAP mapper must fail closed (exit 2), got %d: %s\n",
                result, log);
        goto done;
    }
    if (media_sha256(image, after) != 0 ||
        memcmp(before, after, sizeof(before)) != 0) {
        fprintf(stderr, "SWAP mapper changed the input media\n");
        goto done;
    }
    if (run(argv[1], verify, log, sizeof(log)) != 0) {
        fprintf(stderr, "SWAP.so verification failed after refusal: %s\n", log);
        goto done;
    }
    puts("SWAP V2: format/probe passed; unsupported mapping rejected; media unchanged");
    failed = 0;
done:
    unlink(image);
    rmdir(work);
    return failed;
}
