// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FAT directory catalogue and VFAT name decoder.
 *
 * Placement and mutation policy live in writer.c.  Keeping parsing here makes
 * malformed-directory rejection independently testable and ensures analysis
 * and rewriting consume the same validated catalogue.
 */

#include "fat_directory.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "ld_io.h"
#include "ld_runtime.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/endian.h"
#include "infiltratr/utf8.h"

#define FAT_PROGRAM_NAME "linux-defragger-fat-worker"
#define MAX_RECURSION_DEPTH 128U

typedef struct {
    uint16_t chars[260];
    uint8_t present[20];
    uint8_t checksum;
    unsigned slots;
    bool active;
} LfnState;

static void dirreflist_push(DirRefList *list, DirRef ref) {
    if (list->len == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&list->v, &list->cap, sizeof(*list->v),
                                  list->len + 1U, 128U))
        ld_die("cannot grow FAT directory-reference list");
    list->v[list->len++] = ref;
}

void dirreflist_free(DirRefList *list) {
    free(list->v);
    memset(list, 0, sizeof(*list));
}

static void filelist_push(FileList *list, FileRecord record) {
    if (list->len == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&list->v, &list->cap, sizeof(*list->v),
                                  list->len + 1U, 128U))
        ld_die("cannot grow FAT file list");
    list->v[list->len++] = record;
}

void filelist_free(FileList *list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->v[i].path);
        u32vec_free(&list->v[i].chain);
    }
    free(list->v);
    memset(list, 0, sizeof(*list));
}

size_t chain_fragments(const U32Vec *chain) {
    if (chain->len == 0) return 0;
    size_t fragments = 1;
    for (size_t i = 1; i < chain->len; i++) {
        if (chain->v[i] != chain->v[i - 1] + 1) fragments++;
    }
    return fragments;
}

static void claim_chain(Fat32 *fs, const U32Vec *chain, const char *owner) {
    for (size_t i = 0; i < chain->len; i++) {
        uint32_t cluster = chain->v[i];
        if (ld_bitmap_get(fs->claimed_clusters, cluster)) {
            fprintf(
                stderr,
                "%s: cross-linked cluster %" PRIu32 " while scanning %s\n",
                FAT_PROGRAM_NAME,
                cluster,
                owner
            );
            exit(EXIT_FAILURE);
        }
        ld_bitmap_set(fs->claimed_clusters, cluster, true);
    }
}

static void short_name(const uint8_t entry[32], char out[64]) {
    uint8_t raw[12] = {0};
    size_t raw_len = 0;
    size_t base_end = 8;
    while (base_end != 0 && entry[base_end - 1] == ' ') base_end--;
    size_t extension_end = 3;
    while (extension_end != 0 && entry[8 + extension_end - 1] == ' ') {
        extension_end--;
    }
    for (size_t i = 0; i < base_end; i++) raw[raw_len++] = entry[i];
    if (raw_len != 0 && raw[0] == 0x05) raw[0] = 0xE5;
    if (extension_end != 0) {
        raw[raw_len++] = '.';
        for (size_t i = 0; i < extension_end; i++) {
            raw[raw_len++] = entry[8 + i];
        }
    }

    size_t pos = 0;
    for (size_t i = 0; i < raw_len && pos + 1 < 64; i++) {
        uint8_t ch = raw[i];
        if (ch >= 0x20 && ch < 0x7F) {
            out[pos++] = (char)ch;
        } else if (pos + 4 < 64) {
            int written = snprintf(out + pos, 64 - pos, "\\x%02X", ch);
            if (written < 0) break;
            pos += (size_t)written;
        }
    }
    out[pos] = '\0';
}

static void lfn_reset(LfnState *state) {
    memset(state, 0, sizeof(*state));
    for (size_t i = 0; i < sizeof(state->chars) / sizeof(state->chars[0]); i++) {
        state->chars[i] = UINT16_C(0xFFFF);
    }
}

static uint8_t lfn_short_checksum(const uint8_t entry[32]) {
    uint8_t sum = 0;
    for (size_t i = 0; i < 11; i++) {
        sum = (uint8_t)(
            ((sum & 1U) ? 0x80U : 0U) + (sum >> 1) + entry[i]
        );
    }
    return sum;
}

static void lfn_accept_entry(LfnState *state, const uint8_t entry[32]) {
    unsigned ordinal = entry[0];
    unsigned slot = ordinal & 0x1FU;
    if (slot == 0 || slot > 20) {
        lfn_reset(state);
        return;
    }
    if ((ordinal & 0x40U) != 0) {
        lfn_reset(state);
        state->active = true;
        state->slots = slot;
        state->checksum = entry[13];
    }
    if (
        !state->active
        || slot > state->slots
        || entry[13] != state->checksum
    ) {
        lfn_reset(state);
        return;
    }
    static const uint8_t offsets[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    size_t base = (slot - 1U) * 13U;
    for (size_t i = 0; i < 13; i++) {
        state->chars[base + i] = infiltratr_load_le16(entry + offsets[i]);
    }
    state->present[slot - 1U] = 1;
}

static void utf8_append(
    char *out,
    size_t capacity,
    size_t *position,
    uint32_t codepoint
) {
    if (codepoint == '/' || codepoint == '\\' || codepoint == 0U) {
        codepoint = '_';
    }

    char encoded[4];
    size_t encoded_length = 0U;
    if (!infiltratr_utf8_encode_codepoint(
            codepoint, encoded, sizeof(encoded), &encoded_length)) {
        codepoint = UINT32_C(0xFFFD);
        if (!infiltratr_utf8_encode_codepoint(
                codepoint, encoded, sizeof(encoded), &encoded_length)) {
            return;
        }
    }

    if (*position < capacity &&
        encoded_length <= capacity - *position - 1U) {
        memcpy(out + *position, encoded, encoded_length);
        *position += encoded_length;
    }
}

static char *lfn_name(
    const LfnState *state,
    const uint8_t short_entry[32]
) {
    if (
        !state->active
        || state->slots == 0
        || state->checksum != lfn_short_checksum(short_entry)
    ) {
        return NULL;
    }
    for (unsigned i = 0; i < state->slots; i++) {
        if (!state->present[i]) return NULL;
    }
    char *out = ld_xmalloc(1041);
    size_t position = 0;
    size_t units = (size_t)state->slots * 13U;
    for (size_t i = 0; i < units; i++) {
        uint16_t word = state->chars[i];
        if (word == 0 || word == UINT16_C(0xFFFF)) break;
        uint32_t codepoint = word;
        if (word >= 0xD800 && word <= 0xDBFF && i + 1 < units) {
            uint16_t low = state->chars[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = UINT32_C(0x10000)
                    + (((uint32_t)word - 0xD800U) << 10)
                    + ((uint32_t)low - 0xDC00U);
                i++;
            } else {
                codepoint = UINT32_C(0xFFFD);
            }
        } else if (word >= 0xDC00 && word <= 0xDFFF) {
            codepoint = UINT32_C(0xFFFD);
        }
        utf8_append(out, 1041, &position, codepoint);
    }
    out[position] = '\0';
    if (position == 0) {
        free(out);
        return NULL;
    }
    return out;
}

static char *path_join(const char *parent, const char *name) {
    size_t parent_length = strlen(parent);
    size_t name_length = strlen(name);
    bool slash = (
        parent_length > 0 && parent[parent_length - 1] != '/'
    );
    char *joined = ld_xmalloc(
        parent_length + (slash ? 1 : 0) + name_length + 1
    );
    memcpy(joined, parent, parent_length);
    size_t position = parent_length;
    if (slash) joined[position++] = '/';
    memcpy(joined + position, name, name_length + 1);
    return joined;
}

static bool is_dot_entry(const uint8_t entry[32]) {
    if (entry[0] != '.') return false;
    return entry[1] == ' ' || entry[1] == '.';
}

static void scan_directory(
    Fat32 *fs,
    uint32_t first_cluster,
    const char *path,
    FileList *files,
    DirRefList *dir_refs,
    unsigned depth
) {
    if (depth > MAX_RECURSION_DEPTH) {
        ld_die("directory recursion limit exceeded");
    }
    bool fixed_root = (
        fs->root_is_fixed && first_cluster == 0 && depth == 0
    );
    U32Vec chain = {0};
    uint64_t unit_size = fs->cluster_size;
    size_t units = 0;
    if (fixed_root) {
        unit_size = fs->root_dir_size;
        units = fs->root_dir_size == 0 ? 0 : 1;
    } else {
        if (first_cluster < 2 || first_cluster > fs->max_cluster) {
            ld_die("invalid directory cluster");
        }
        if (ld_bitmap_get(fs->visited_dirs, first_cluster)) {
            ld_die("directory cycle or cross-link detected");
        }
        ld_bitmap_set(fs->visited_dirs, first_cluster, true);
        chain = fat32_read_chain(fs, first_cluster);
        units = chain.len;
    }

    uint8_t *buffer = ld_xmalloc((size_t)unit_size);
    bool end_directory = false;
    LfnState lfn;
    lfn_reset(&lfn);
    for (size_t chain_index = 0;
         chain_index < units && !end_directory;
         chain_index++) {
        uint64_t cluster_position = fixed_root
            ? fs->root_dir_offset
            : cluster_offset(fs, chain.v[chain_index]);
        if (
            ld_pread_full(
                fs->dev.fd,
                buffer,
                (size_t)unit_size,
                cluster_position
            ) != (ssize_t)unit_size
        ) {
            ld_die_errno("read directory data");
        }
        for (uint64_t position = 0;
             position + 32 <= unit_size;
             position += 32) {
            uint8_t *entry = buffer + position;
            if (entry[0] == 0x00) {
                end_directory = true;
                lfn_reset(&lfn);
                break;
            }
            if (entry[0] == 0xE5) {
                lfn_reset(&lfn);
                continue;
            }
            uint8_t attributes = entry[11];
            if (attributes == 0x0F) {
                lfn_accept_entry(&lfn, entry);
                continue;
            }
            if ((attributes & 0x08) != 0) {
                lfn_reset(&lfn);
                continue;
            }
            uint32_t first = infiltratr_load_le16(entry + 26);
            if (fs->fat_type == FAT_TYPE_32) {
                first |= (uint32_t)infiltratr_load_le16(entry + 20) << 16;
            }
            first &= fat_mask(fs);
            if (dir_refs != NULL && first != 0) {
                dirreflist_push(
                    dir_refs,
                    (DirRef){
                        .offset = cluster_position + position,
                        .target_cluster = first,
                    }
                );
            }
            if (is_dot_entry(entry)) {
                lfn_reset(&lfn);
                continue;
            }

            char short_buffer[64];
            char *long_name = lfn_name(&lfn, entry);
            lfn_reset(&lfn);
            const char *name = long_name;
            if (name == NULL) {
                short_name(entry, short_buffer);
                name = short_buffer;
            }
            if (name[0] == '\0') {
                free(long_name);
                continue;
            }
            uint32_t size = infiltratr_load_le32(entry + 28);
            bool is_directory = (attributes & 0x10) != 0;
            char *full_path = path_join(path, name);
            free(long_name);
            FileRecord record = {
                .path = full_path,
                .dirent_offset = cluster_position + position,
                .first_cluster = first,
                .size_bytes = size,
                .attr = attributes,
                .is_dir = is_directory,
            };
            if (first != 0) {
                record.chain = fat32_read_chain(fs, first);
                record.fragments = chain_fragments(&record.chain);
                claim_chain(fs, &record.chain, full_path);
            }
            if (!is_directory) {
                size_t expected = size == 0
                    ? 0
                    : (size_t)(
                        ((uint64_t)size + fs->cluster_size - 1)
                        / fs->cluster_size
                    );
                if (
                    (size == 0 && first != 0)
                    || (size != 0 && first == 0)
                    || record.chain.len != expected
                ) {
                    fprintf(
                        stderr,
                        "%s: file size and cluster-chain length disagree for %s\n",
                        FAT_PROGRAM_NAME,
                        full_path
                    );
                    exit(EXIT_FAILURE);
                }
            }
            filelist_push(files, record);
            if (is_directory && first != 0) {
                scan_directory(
                    fs,
                    first,
                    full_path,
                    files,
                    dir_refs,
                    depth + 1
                );
            }
        }
    }
    free(buffer);
    u32vec_free(&chain);
}

U32Vec filesystem_root_chain(Fat32 *fs) {
    if (fs->root_is_fixed) {
        return (U32Vec){0};
    }
    return fat32_read_chain(fs, fs->root_cluster);
}

FileList scan_files(Fat32 *fs, DirRefList *dir_refs) {
    size_t guard_bytes = 0U;
    if (!ld_bitmap_size((uint64_t)fs->max_cluster + 1U, &guard_bytes))
        ld_die("FAT traversal guard size overflow");
    memset(fs->visited_dirs, 0, guard_bytes);
    memset(fs->claimed_clusters, 0, guard_bytes);
    if (dir_refs != NULL) {
        dirreflist_free(dir_refs);
    }
    if (!fs->root_is_fixed) {
        U32Vec root_chain = filesystem_root_chain(fs);
        claim_chain(fs, &root_chain, "<root directory>");
        u32vec_free(&root_chain);
    }
    FileList files = {0};
    scan_directory(
        fs,
        fs->root_is_fixed ? 0 : fs->root_cluster,
        "",
        &files,
        dir_refs,
        0
    );

    for (uint32_t cluster = 2; cluster <= fs->max_cluster; cluster++) {
        uint32_t value = fat_value(fs, cluster);
        if (value == 0 || value == fat_bad_value(fs)) continue;
        if (
            value >= fat_reserved_min(fs)
            && value < fat_eoc_min(fs)
        ) {
            fprintf(
                stderr,
                "%s: reserved FAT value at cluster %" PRIu32 "\n",
                FAT_PROGRAM_NAME,
                cluster
            );
            exit(EXIT_FAILURE);
        }
        if (!ld_bitmap_get(fs->claimed_clusters, cluster)) {
            fprintf(
                stderr,
                "%s: allocated but unreferenced cluster %" PRIu32
                " detected; repair lost chains before defragmenting\n",
                FAT_PROGRAM_NAME,
                cluster
            );
            exit(EXIT_FAILURE);
        }
    }
    return files;
}

