// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LINUX_DEFRAGGER_MINIX_NATIVE_H
#define LINUX_DEFRAGGER_MINIX_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MinixSummary {
    uint16_t magic;
    uint32_t inode_count;
    uint32_t zone_count;
    uint16_t imap_blocks;
    uint16_t zmap_blocks;
    uint32_t first_data_zone;
    uint16_t log_zone_size;
    uint32_t max_size;
    uint32_t block_size;
    uint64_t zone_size;
    unsigned int version;
    bool long_names;
    bool little_endian;
} MinixSummary;

typedef struct MinixAnalysis {
    MinixSummary summary;
    uint64_t physical_bytes;
    uint64_t filesystem_bytes;
    uint64_t total_units;
    uint64_t free_zones;
    uint64_t used_zones;
    uint64_t regular_files;
    uint64_t directories;
    uint64_t fragmented_files;
    uint64_t fragmented_directories;
} MinixAnalysis;

typedef struct MinixMapCell {
    uint64_t start;
    uint64_t end;
    uint64_t free_count;
    uint64_t used_count;
    uint64_t outside_count;
    uint64_t fragmented_count;
    uint64_t directory_count;
} MinixMapCell;

int minix_read_summary(const char *path, MinixSummary *summary,
                       char *error, size_t error_size);
int minix_analyse(const char *path, MinixAnalysis *analysis,
                  MinixMapCell *cells, uint64_t cell_count,
                  char *error, size_t error_size);
int minix_build_stage(const char *source, const char *stage,
                      bool growth, unsigned growth_percent,
                      uint64_t *commit_bytes,
                      char *error, size_t error_size);
int minix_verify_layout(const char *path, bool growth,
                        unsigned growth_percent,
                        char *error, size_t error_size);
bool minix_probe(const char *path);
const char *minix_variant_name(const MinixSummary *summary);
const char *minix_byte_order_name(const MinixSummary *summary);

#endif
