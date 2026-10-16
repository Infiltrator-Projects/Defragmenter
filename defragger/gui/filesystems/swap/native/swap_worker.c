// SPDX-License-Identifier: GPL-3.0-or-later
#include "swap_native.h"
#include "version.h"
#include "infiltratr/core.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG "defragmenter-swap-worker"

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "Usage: %s --version | identify DEVICE | analyse-json DEVICE | "
                  "map DEVICE --cells COUNT\n",
                  PROG);
}

static int parse_cells(const char *text, uint64_t *cells)
{
    return infiltratr_parse_u64_range(text, 10U, 1U, UINT64_MAX, cells)
        ? 0 : -1;
}

static void print_json_bytes(const uint8_t *bytes, size_t length)
{
    (void)putchar('"');
    for (size_t index = 0U; index < length; ++index) {
        const unsigned int value = bytes[index];
        if (value == (unsigned int)'"') {
            (void)fputs("\\\"", stdout);
        } else if (value == (unsigned int)'\\') {
            (void)fputs("\\\\", stdout);
        } else if (value >= 0x20U && value <= 0x7eU) {
            (void)putchar((int)value);
        } else {
            (void)printf("\\u%04x", value);
        }
    }
    (void)putchar('"');
}

static void print_summary_json(const LdSwapSummary *summary, int detailed)
{
    (void)printf("{\"filesystem\":\"swap\",\"format\":\"%s\","
                 "\"page_size\":%u,\"byte_order\":\"%s\"",
                 ld_swap_format_name(summary),
                 (unsigned int)summary->page_size,
                 ld_swap_byte_order_name(summary));
    if (detailed != 0) {
        char uuid[37];
        ld_swap_uuid_string(summary, uuid);
        (void)printf(",\"version\":%u,\"last_page\":%u,"
                     "\"filesystem_pages\":%llu,\"usable_pages\":%llu,"
                     "\"bad_pages\":%u,\"container_bytes\":%llu,"
                     "\"filesystem_bytes\":%llu,\"outside_bytes\":%llu,"
                     "\"uuid\":\"%s\",\"label\":",
                     (unsigned int)summary->version,
                     (unsigned int)summary->last_page,
                     (unsigned long long)summary->filesystem_pages,
                     (unsigned long long)summary->usable_pages,
                     (unsigned int)summary->bad_page_count,
                     (unsigned long long)summary->container_bytes,
                     (unsigned long long)summary->filesystem_bytes,
                     (unsigned long long)summary->outside_bytes,
                     uuid);
        print_json_bytes(summary->label, summary->label_length);
    }
    (void)puts("}");
}

static uint64_t cell_overlap(uint64_t start, uint64_t end,
                             uint64_t range_start, uint64_t range_end)
{
    const uint64_t left = start > range_start ? start : range_start;
    const uint64_t right = end < range_end ? end : range_end;
    return right > left ? right - left : 0U;
}

static uint64_t bad_pages_in_cell(const LdSwapSummary *summary,
                                  uint64_t start, uint64_t end)
{
    const uint64_t fs_end =
        end < summary->filesystem_pages ? end : summary->filesystem_pages;
    if (fs_end <= start)
        return 0U;
    return ld_swap_bad_pages_in_range(summary, start, fs_end);
}

static void print_map_details(const LdSwapSummary *summary,
                              const LdSwapRuntimeUsage *runtime,
                              const char *proc_source)
{
    char uuid[37];
    ld_swap_uuid_string(summary, uuid);
    (void)printf("\"details\":{\"format\":\"%s\",\"signature\":\"%s\","
                 "\"version\":%u,\"byte_order\":\"%s\","
                 "\"page_size\":%u,\"last_page\":%u,"
                 "\"filesystem_pages\":%llu,\"usable_pages\":%llu,"
                 "\"bad_pages\":%u,\"uuid\":\"%s\",\"label\":",
                 ld_swap_format_name(summary),
                 summary->format == LD_SWAP_FORMAT_CURRENT ? "SWAPSPACE2" : "SWAP-SPACE",
                 (unsigned int)summary->version,
                 ld_swap_byte_order_name(summary),
                 (unsigned int)summary->page_size,
                 (unsigned int)summary->last_page,
                 (unsigned long long)summary->filesystem_pages,
                 (unsigned long long)summary->usable_pages,
                 (unsigned int)summary->bad_page_count,
                 uuid);
    print_json_bytes(summary->label, summary->label_length);
    (void)printf(",\"container_bytes\":%llu,\"filesystem_bytes\":%llu,"
                 "\"outside_bytes\":%llu,\"active\":%s",
                 (unsigned long long)summary->container_bytes,
                 (unsigned long long)summary->filesystem_bytes,
                 (unsigned long long)summary->outside_bytes,
                 runtime->active ? "true" : "false");
    if (runtime->active) {
        (void)printf(",\"usage_source\":\"%s\",\"swap_type\":\"%s\","
                     "\"priority\":%d,\"runtime_total_bytes\":%llu,"
                     "\"runtime_used_bytes\":%llu,\"runtime_free_bytes\":%llu,"
                     "\"usage_is_aggregate\":true,"
                     "\"note\":\"Linux exposes aggregate active swap usage but not the physical locations of occupied swap slots\"",
                     proc_source, runtime->type, runtime->priority,
                     (unsigned long long)runtime->total_bytes,
                     (unsigned long long)runtime->used_bytes,
                     (unsigned long long)(runtime->total_bytes - runtime->used_bytes));
    } else if (summary->format == LD_SWAP_FORMAT_CURRENT) {
        (void)fputs(",\"usage_source\":\"inactive swap area\","
                    "\"usage_is_aggregate\":false,"
                    "\"note\":\"inactive Linux swap has no occupied slots; header and bad pages are reserved and the declared tail is outside the swap area\"",
                    stdout);
    } else {
        (void)fputs(",\"usage_source\":\"legacy swap signature\","
                    "\"usage_is_aggregate\":false,"
                    "\"note\":\"legacy SWAP-SPACE is identified conservatively; modern structured Linux swap header fields are not assumed\"",
                    stdout);
    }
    (void)putchar('}');
}

static void print_map_json(const LdSwapSummary *summary,
                           uint64_t requested_cells,
                           const LdSwapRuntimeUsage *runtime,
                           const char *proc_source)
{
    uint64_t total_units = summary->container_bytes / (uint64_t)summary->page_size;
    if (summary->container_bytes % (uint64_t)summary->page_size != 0U)
        total_units++;
    if (total_units == 0U)
        total_units = 1U;
    uint64_t cell_count = requested_cells < total_units ? requested_cells : total_units;
    if (cell_count == 0U)
        cell_count = 1U;

    const bool current = summary->format == LD_SWAP_FORMAT_CURRENT;
    const bool active = current && runtime->active;
    const char *accuracy = current && !active ? "exact" : "summary";
    const uint64_t bad_bytes = (uint64_t)summary->bad_page_count * summary->page_size;
    const uint64_t reserved_bytes =
        current ? (uint64_t)summary->page_size + bad_bytes : 0U;
    uint64_t free_bytes = 0U;
    uint64_t used_bytes = 0U;
    uint64_t unknown_bytes = summary->filesystem_bytes;
    if (current && !active) {
        free_bytes = summary->usable_pages * summary->page_size;
        unknown_bytes = 0U;
    } else if (active) {
        free_bytes = runtime->total_bytes - runtime->used_bytes;
        used_bytes = runtime->used_bytes;
        unknown_bytes = summary->usable_pages * summary->page_size;
    }

    (void)printf("{\"schema\":1,\"backend\":\"read-only-domain\","
                 "\"filesystem\":\"swap\",\"map_accuracy\":\"%s\","
                 "\"unit_size\":%u,\"total_units\":%llu,\"cell_count\":%llu,"
                 "\"total_bytes\":%llu,\"free_bytes\":%llu,\"used_bytes\":%llu,"
                 "\"unknown_bytes\":%llu,\"reserved_bytes\":%llu,"
                 "\"filesystem_bytes\":%llu,\"outside_bytes\":%llu,\"cells\":[",
                 accuracy, (unsigned int)summary->page_size,
                 (unsigned long long)total_units,
                 (unsigned long long)cell_count,
                 (unsigned long long)summary->container_bytes,
                 (unsigned long long)free_bytes,
                 (unsigned long long)used_bytes,
                 (unsigned long long)unknown_bytes,
                 (unsigned long long)reserved_bytes,
                 (unsigned long long)summary->filesystem_bytes,
                 (unsigned long long)summary->outside_bytes);

    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t start = (index * total_units) / cell_count;
        const uint64_t end = ((index + 1U) * total_units) / cell_count;
        const uint64_t length = end - start;
        uint64_t outside = 0U;
        uint64_t bad = 0U;
        uint64_t free = 0U;
        uint64_t unknown = length;
        if (current) {
            outside = cell_overlap(start, end, summary->filesystem_pages, total_units);
            bad = bad_pages_in_cell(summary, start, end);
            const uint64_t header = start == 0U ? 1U : 0U;
            const uint64_t metadata = bad + header;
            const uint64_t inside = length - outside;
            bad = metadata;
            if (active) {
                unknown = inside - metadata;
            } else {
                free = inside - metadata;
                unknown = 0U;
            }
        } else {
            outside = cell_overlap(start, end, summary->filesystem_pages, total_units);
            unknown = length - outside;
        }
        if (index != 0U)
            (void)putchar(',');
        (void)printf("{\"start\":%llu,\"end\":%llu,\"free\":%llu,"
                     "\"used\":0,\"unknown\":%llu,\"bad\":%llu,"
                     "\"outside\":%llu,\"fragmented\":0,\"directory\":0}",
                     (unsigned long long)start,
                     (unsigned long long)(end - 1U),
                     (unsigned long long)free,
                     (unsigned long long)unknown,
                     (unsigned long long)bad,
                     (unsigned long long)outside);
    }
    (void)fputs("],", stdout);
    print_map_details(summary, runtime, proc_source);
    (void)puts("}");
}

static int read_summary(const char *path, LdSwapSummary *summary)
{
    char error[256];
    if (ld_swap_read_summary(path, summary, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", PROG, error);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("%s %s\n", PROG, LD_VERSION);
        return 0;
    }

    if (argc == 5 && strcmp(argv[1], "map") == 0 &&
        strcmp(argv[3], "--cells") == 0) {
        uint64_t cells = 0U;
        if (parse_cells(argv[4], &cells) != 0) {
            (void)fprintf(stderr, "%s: invalid cell count\n", PROG);
            return 2;
        }
        LdSwapSummary summary;
        if (read_summary(argv[2], &summary) != 0)
            return 1;
        LdSwapRuntimeUsage runtime;
        char runtime_error[256];
        const char *proc_source = getenv("DEFRAGMENTER_PROC_SWAPS");
        if (proc_source == NULL || *proc_source == '\0')
            proc_source = "/proc/swaps";
        const int runtime_result =
            ld_swap_runtime_usage(argv[2], proc_source, &runtime,
                                  runtime_error, sizeof(runtime_error));
        if (runtime_result < 0) {
            (void)fprintf(stderr, "%s: %s\n", PROG, runtime_error);
            ld_swap_summary_destroy(&summary);
            return 1;
        }
        print_map_json(&summary, cells, &runtime, proc_source);
        ld_swap_summary_destroy(&summary);
        return 0;
    }

    if (argc != 3) {
        usage(stderr);
        return 2;
    }
    LdSwapSummary summary;
    if (read_summary(argv[2], &summary) != 0)
        return 1;
    if (strcmp(argv[1], "identify") == 0) {
        print_summary_json(&summary, 0);
        ld_swap_summary_destroy(&summary);
        return 0;
    }
    if (strcmp(argv[1], "analyse-json") == 0) {
        print_summary_json(&summary, 1);
        ld_swap_summary_destroy(&summary);
        return 0;
    }
    ld_swap_summary_destroy(&summary);
    usage(stderr);
    return 2;
}
