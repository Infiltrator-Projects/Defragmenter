// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEFRAGMENTER_SWAP_NATIVE_H
#define DEFRAGMENTER_SWAP_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LD_SWAP_UUID_BYTES 16U
#define LD_SWAP_LABEL_BYTES 16U
#define LD_SWAP_PROC_TYPE_BYTES 32U

typedef enum {
    LD_SWAP_FORMAT_LEGACY = 0,
    LD_SWAP_FORMAT_CURRENT = 1,
} LdSwapFormat;

typedef struct {
    LdSwapFormat format;
    uint32_t page_size;
    bool little_endian;
    uint32_t version;
    uint32_t last_page;
    uint64_t filesystem_pages;
    uint64_t usable_pages;
    uint32_t bad_page_count;
    uint32_t *bad_pages;
    uint8_t uuid[LD_SWAP_UUID_BYTES];
    uint8_t label[LD_SWAP_LABEL_BYTES];
    size_t label_length;
    bool uuid_present;
    bool regular_file;
    uint64_t container_bytes;
    uint64_t filesystem_bytes;
    uint64_t outside_bytes;
} LdSwapSummary;

typedef struct {
    bool active;
    uint64_t total_bytes;
    uint64_t used_bytes;
    int priority;
    char type[LD_SWAP_PROC_TYPE_BYTES];
} LdSwapRuntimeUsage;

int ld_swap_read_summary(const char *path, LdSwapSummary *summary,
                         char *error, size_t error_size);
void ld_swap_summary_destroy(LdSwapSummary *summary);
bool ld_swap_probe(const char *path);
const char *ld_swap_format_name(const LdSwapSummary *summary);
const char *ld_swap_byte_order_name(const LdSwapSummary *summary);
void ld_swap_uuid_string(const LdSwapSummary *summary, char output[37]);
uint64_t ld_swap_bad_pages_in_range(const LdSwapSummary *summary,
                                    uint64_t start_page,
                                    uint64_t end_page);
int ld_swap_runtime_usage(const char *path, const char *proc_swaps_path,
                          LdSwapRuntimeUsage *usage,
                          char *error, size_t error_size);

#endif
