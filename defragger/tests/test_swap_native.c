// SPDX-License-Identifier: GPL-3.0-or-later
#include "swap_native.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",              \
                          __FILE__, __LINE__, #condition);                       \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

#define TEST_PAGE 4096U

static void put_u32(unsigned char *buffer, size_t offset, uint32_t value,
                    int little_endian)
{
    if (little_endian != 0) {
        buffer[offset] = (unsigned char)(value & 0xffU);
        buffer[offset + 1U] = (unsigned char)((value >> 8) & 0xffU);
        buffer[offset + 2U] = (unsigned char)((value >> 16) & 0xffU);
        buffer[offset + 3U] = (unsigned char)((value >> 24) & 0xffU);
    } else {
        buffer[offset] = (unsigned char)((value >> 24) & 0xffU);
        buffer[offset + 1U] = (unsigned char)((value >> 16) & 0xffU);
        buffer[offset + 2U] = (unsigned char)((value >> 8) & 0xffU);
        buffer[offset + 3U] = (unsigned char)(value & 0xffU);
    }
}

static int make_current(const char *path, uint32_t page_size,
                        uint32_t physical_pages, uint32_t last_page,
                        int little_endian, const uint32_t *bad_pages,
                        uint32_t bad_count)
{
    const int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    const off_t bytes = (off_t)((uint64_t)page_size * physical_pages);
    if (ftruncate(fd, bytes) != 0) {
        (void)close(fd);
        return -1;
    }
    unsigned char *page = calloc(1U, page_size);
    if (page == NULL) {
        (void)close(fd);
        return -1;
    }
    put_u32(page, 1024U, 1U, little_endian);
    put_u32(page, 1028U, last_page, little_endian);
    put_u32(page, 1032U, bad_count, little_endian);
    for (size_t index = 0U; index < 16U; ++index)
        page[1036U + index] = (unsigned char)(index + 1U);
    memcpy(page + 1052U, "Swap Test", 9U);
    for (uint32_t index = 0U; index < bad_count; ++index)
        put_u32(page, 1536U + (size_t)index * 4U, bad_pages[index], little_endian);
    memcpy(page + page_size - 10U, "SWAPSPACE2", 10U);
    const ssize_t written = pwrite(fd, page, page_size, 0);
    const int saved_errno = errno;
    free(page);
    (void)close(fd);
    errno = saved_errno;
    return written == (ssize_t)page_size ? 0 : -1;
}

static int make_legacy(const char *path, uint32_t physical_pages)
{
    const int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)((uint64_t)TEST_PAGE * physical_pages)) != 0) {
        (void)close(fd);
        return -1;
    }
    unsigned char page[TEST_PAGE];
    memset(page, 0, sizeof(page));
    memcpy(page + TEST_PAGE - 10U, "SWAP-SPACE", 10U);
    const ssize_t written = pwrite(fd, page, sizeof(page), 0);
    const int saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return written == (ssize_t)sizeof(page) ? 0 : -1;
}

static void test_current_little(void)
{
    char path[] = "/tmp/defragmenter-swap-le.XXXXXX";
    const int temp = mkstemp(path);
    CHECK(temp >= 0);
    (void)close(temp);
    const uint32_t bad[] = {7U, 3U};
    CHECK(make_current(path, 4096U, 16U, 11U, 1, bad, 2U) == 0);

    LdSwapSummary summary;
    char error[256];
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.format == LD_SWAP_FORMAT_CURRENT);
    CHECK(summary.page_size == 4096U);
    CHECK(summary.little_endian);
    CHECK(summary.version == 1U);
    CHECK(summary.last_page == 11U);
    CHECK(summary.filesystem_pages == 12U);
    CHECK(summary.usable_pages == 9U);
    CHECK(summary.bad_page_count == 2U);
    CHECK(summary.bad_pages[0] == 3U);
    CHECK(summary.bad_pages[1] == 7U);
    CHECK(summary.container_bytes == 16ULL * 4096ULL);
    CHECK(summary.filesystem_bytes == 12ULL * 4096ULL);
    CHECK(summary.outside_bytes == 4ULL * 4096ULL);
    CHECK(summary.label_length == 9U);
    CHECK(memcmp(summary.label, "Swap Test", 9U) == 0);
    CHECK(summary.uuid_present);
    char uuid[37];
    ld_swap_uuid_string(&summary, uuid);
    CHECK(strcmp(uuid, "01020304-0506-0708-090a-0b0c0d0e0f10") == 0);
    CHECK(ld_swap_bad_pages_in_range(&summary, 0U, 4U) == 1U);
    CHECK(ld_swap_bad_pages_in_range(&summary, 4U, 8U) == 1U);
    CHECK(ld_swap_bad_pages_in_range(&summary, 8U, 12U) == 0U);
    ld_swap_summary_destroy(&summary);
    (void)unlink(path);
}

static void test_current_big(void)
{
    char path[] = "/tmp/defragmenter-swap-be.XXXXXX";
    const int temp = mkstemp(path);
    CHECK(temp >= 0);
    (void)close(temp);
    const uint32_t bad[] = {5U};
    CHECK(make_current(path, 8192U, 14U, 12U, 0, bad, 1U) == 0);

    LdSwapSummary summary;
    char error[256];
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.page_size == 8192U);
    CHECK(!summary.little_endian);
    CHECK(summary.last_page == 12U);
    CHECK(summary.filesystem_pages == 13U);
    CHECK(summary.usable_pages == 11U);
    CHECK(summary.bad_pages[0] == 5U);
    CHECK(strcmp(ld_swap_byte_order_name(&summary), "big") == 0);
    ld_swap_summary_destroy(&summary);
    (void)unlink(path);
}

static void test_legacy_is_not_misparsed(void)
{
    char path[] = "/tmp/defragmenter-swap-v0.XXXXXX";
    const int temp = mkstemp(path);
    CHECK(temp >= 0);
    (void)close(temp);
    CHECK(make_legacy(path, 20U) == 0);

    LdSwapSummary summary;
    char error[256];
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) == 0);
    CHECK(summary.format == LD_SWAP_FORMAT_LEGACY);
    CHECK(summary.version == 0U);
    CHECK(summary.page_size == TEST_PAGE);
    CHECK(strcmp(ld_swap_format_name(&summary), "linux-v0-legacy") == 0);
    CHECK(strcmp(ld_swap_byte_order_name(&summary), "unknown") == 0);
    CHECK(summary.bad_page_count == 0U);
    CHECK(!summary.uuid_present);
    CHECK(summary.label_length == 0U);
    ld_swap_summary_destroy(&summary);
    (void)unlink(path);
}

static void test_invalid_headers(void)
{
    char path[] = "/tmp/defragmenter-swap-invalid.XXXXXX";
    int temp = mkstemp(path);
    CHECK(temp >= 0);
    (void)close(temp);
    const uint32_t bad[] = {3U};
    CHECK(make_current(path, 4096U, 16U, 11U, 1, bad, 1U) == 0);

    int fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    unsigned char wrong_version[4] = {2U, 0U, 0U, 0U};
    CHECK(pwrite(fd, wrong_version, sizeof(wrong_version), 1024) ==
           (ssize_t)sizeof(wrong_version));
    (void)close(fd);
    LdSwapSummary summary;
    char error[256];
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) != 0);

    CHECK(make_current(path, 4096U, 8U, 11U, 1, bad, 1U) == 0);
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) != 0);

    const uint32_t bad_zero[] = {0U};
    CHECK(make_current(path, 4096U, 16U, 11U, 1, bad_zero, 1U) == 0);
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) != 0);

    const uint32_t duplicate[] = {3U, 3U};
    CHECK(make_current(path, 4096U, 16U, 11U, 1, duplicate, 2U) == 0);
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) != 0);

    CHECK(make_current(path, 4096U, 16U, 11U, 1, bad, 1U) == 0);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    unsigned char tux[8] = {0xedU, 0xc3U, 0x02U, 0xe9U, 0x98U, 0x56U, 0xe5U, 0x0cU};
    CHECK(pwrite(fd, tux, sizeof(tux), 0) == (ssize_t)sizeof(tux));
    (void)close(fd);
    CHECK(ld_swap_read_summary(path, &summary, error, sizeof(error)) != 0);
    (void)unlink(path);
}

static void encode_proc_path(const char *path, char *encoded, size_t size)
{
    size_t out = 0U;
    for (size_t in = 0U; path[in] != '\0' && out + 1U < size; ++in) {
        if (path[in] == ' ') {
            CHECK(out + 4U < size);
            memcpy(encoded + out, "\\040", 4U);
            out += 4U;
        } else {
            encoded[out++] = path[in];
        }
    }
    encoded[out] = '\0';
}

static void test_proc_swaps_runtime(void)
{
    char target[] = "/tmp/linux defragger swap runtime.XXXXXX";
    int fd = mkstemp(target);
    CHECK(fd >= 0);
    (void)close(fd);
    char proc_path[] = "/tmp/defragmenter-proc-swaps.XXXXXX";
    fd = mkstemp(proc_path);
    CHECK(fd >= 0);
    FILE *proc = fdopen(fd, "w");
    CHECK(proc != NULL);
    char encoded[4096];
    encode_proc_path(target, encoded, sizeof(encoded));
    CHECK(fprintf(proc, "Filename Type Size Used Priority\n") > 0);
    CHECK(fprintf(proc, "%s file 12345 2345 -7\n", encoded) > 0);
    CHECK(fclose(proc) == 0);

    LdSwapRuntimeUsage usage;
    char error[256];
    CHECK(ld_swap_runtime_usage(target, proc_path, &usage, error, sizeof(error)) == 1);
    CHECK(usage.active);
    CHECK(usage.total_bytes == 12345ULL * 1024ULL);
    CHECK(usage.used_bytes == 2345ULL * 1024ULL);
    CHECK(usage.priority == -7);
    CHECK(strcmp(usage.type, "file") == 0);
    CHECK(ld_swap_runtime_usage("/definitely/not/this", proc_path,
                                 &usage, error, sizeof(error)) == 0);
    CHECK(!usage.active);
    (void)unlink(proc_path);
    (void)unlink(target);
}

int main(void)
{
    test_current_little();
    test_current_big();
    test_legacy_is_not_misparsed();
    test_invalid_headers();
    test_proc_swaps_runtime();
    (void)puts("native swap parser tests passed");
    return 0;
}
