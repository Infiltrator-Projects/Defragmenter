// SPDX-License-Identifier: GPL-3.0-or-later
#define _FILE_OFFSET_BITS 64
#include "affs_native.h"

#include "ld_device.h"
#include "ld_io.h"

#include "infiltratr/endian.h"
#include "infiltratr/arithmetic.h"
#include "infiltratr/posix_io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BS 512U
#define LONGS 128U
#define PTRS 72U
#define T_SHORT 2U
#define T_DATA 8U
#define T_LIST 16U
#define ST_ROOT 1U
#define ST_DIR 2U
#define ST_FILE 0xfffffffdU
#define AFFS_IO_BATCH_BLOCKS 8192U
#define AFFS_MAX_DIRECTORY_DEPTH 256U
#define AFFS_IO_BATCH_BYTES ((size_t)AFFS_IO_BATCH_BLOCKS * BS)

static uint32_t lng(const unsigned char *b, int i) {
    if (i < 0) i += (int)LONGS;
    return infiltratr_load_be32(b + (size_t)i * 4U);
}

static void plng(unsigned char *b, int i, uint32_t x) {
    if (i < 0) i += (int)LONGS;
    infiltratr_store_be32(b + (size_t)i * 4U, x);
}

void affs_set_error(char **e, const char *fmt, ...) {
    if (!e) return;
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) s = NULL;
    va_end(ap);
    free(*e);
    *e = s;
}

static int push(AffsU32Vec *a, uint32_t x) {
    if (a->n == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&a->v, &a->cap, sizeof(*a->v),
                                  a->n + 1U, 16U))
        return -1;
    a->v[a->n++] = x;
    return 0;
}

static int fpush(AffsFileVec *a, AffsFile x) {
    if (a->n == SIZE_MAX ||
        !infiltratr_array_reserve((void **)&a->v, &a->cap, sizeof(*a->v),
                                  a->n + 1U, 32U))
        return -1;
    a->v[a->n++] = x;
    return 0;
}

static ssize_t pread_full_local(int fd, void *buffer, size_t length, uint64_t offset) {
    if (length > (size_t)SSIZE_MAX) { errno = EOVERFLOW; return -1; }
    return infiltratr_pread_full(fd, buffer, length, offset) == 0
        ? (ssize_t)length : -1;
}

static ssize_t pwrite_full_local(int fd, const void *buffer, size_t length, uint64_t offset) {
    if (length > (size_t)SSIZE_MAX) { errno = EOVERFLOW; return -1; }
    return infiltratr_pwrite_full(fd, buffer, length, offset) == 0
        ? (ssize_t)length : -1;
}

static int rd(int fd, uint32_t n, unsigned char b[BS], char **e) {
    ssize_t r = pread_full_local(fd, b, BS, (uint64_t)n * BS);
    if (r != (ssize_t)BS) {
        affs_set_error(e, "cannot read Amiga block %u: %s", n,
                       r < 0 ? strerror(errno) : "short read");
        return -1;
    }
    return 0;
}

static int wr(int fd, uint32_t n, const unsigned char b[BS], char **e) {
    ssize_t r = pwrite_full_local(fd, b, BS, (uint64_t)n * BS);
    if (r != (ssize_t)BS) {
        affs_set_error(e, "cannot write Amiga block %u: %s", n,
                       r < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

static uint32_t csum(const unsigned char b[BS], int skip) {
    uint32_t s = 0;
    for (int i = 0; i < (int)LONGS; ++i) {
        if (i != skip) s += lng(b, i);
    }
    return (uint32_t)(0U - s);
}

static bool block_ok(const unsigned char b[BS], uint32_t type, uint32_t subtype, int chk) {
    return lng(b, 0) == type && lng(b, -1) == subtype && lng(b, chk) == csum(b, chk);
}

static void fixsum(unsigned char b[BS], int chk) {
    plng(b, chk, 0);
    plng(b, chk, csum(b, chk));
}

static void free_vec(AffsU32Vec *a) {
    free(a->v);
    memset(a, 0, sizeof(*a));
}

void affs_close(AffsVolume *v) {
    if (!v) return;
    if (v->fd >= 0) close(v->fd);
    for (size_t i = 0; i < v->files.n; ++i) {
        free_vec(&v->files.v[i].data);
        free_vec(&v->files.v[i].lists);
    }
    free(v->files.v);
    free_vec(&v->bitmap_blocks);
    free_vec(&v->bitmap_ext_blocks);
    free_vec(&v->directory_blocks);
    free(v->free_map);
    free(v->fixed_map);
    memset(v, 0, sizeof(*v));
    v->fd = -1;
}

size_t affs_fragments(const AffsU32Vec *a) {
    if (!a || !a->n) return 0;
    size_t n = 1;
    for (size_t i = 1; i < a->n; ++i) {
        if (a->v[i] != a->v[i - 1] + 1U) ++n;
    }
    return n;
}

static int scan_file(AffsVolume *v, uint32_t blk, const unsigned char hb[BS], char **e) {
    AffsFile f = {0};
    f.header = blk;
    f.byte_size = lng(hb, -47);
    uint32_t count = lng(hb, 2);
    if (count > PTRS) {
        affs_set_error(e, "Amiga file header %u has impossible block count %u", blk, count);
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t x = lng(hb, -51 - (int)i);
        if (!x || x >= v->blocks || push(&f.data, x)) {
            affs_set_error(e, "invalid Amiga file data pointer in header %u", blk);
            goto fail;
        }
    }

    uint32_t ext = lng(hb, -2);
    size_t guard = 0;
    while (ext) {
        if (ext >= v->blocks || ++guard > v->blocks) {
            affs_set_error(e, "invalid Amiga file-list chain from header %u", blk);
            goto fail;
        }
        unsigned char b[BS];
        if (rd(v->fd, ext, b, e) || !block_ok(b, T_LIST, ST_FILE, 5) || lng(b, 1) != ext) {
            affs_set_error(e, "invalid Amiga file-list block %u", ext);
            goto fail;
        }
        if (push(&f.lists, ext)) goto oom;
        ld_bitmap_set(v->fixed_map, ext, true);
        uint32_t n = lng(b, 2);
        if (n > PTRS) {
            affs_set_error(e, "Amiga file-list block %u has impossible count", ext);
            goto fail;
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t x = lng(b, -51 - (int)i);
            if (!x || x >= v->blocks || push(&f.data, x)) {
                affs_set_error(e, "invalid Amiga file-list pointer in block %u", ext);
                goto fail;
            }
        }
        ext = lng(b, -2);
    }

    uint32_t payload = v->ffs ? BS : (BS - 24U);
    uint32_t want = (f.byte_size + payload - 1U) / payload;
    if (f.data.n != want) {
        affs_set_error(e, "Amiga file header %u references %zu data blocks but size requires %u",
                       blk, f.data.n, want);
        goto fail;
    }
    for (size_t i = 0; i < f.data.n; ++i) {
        if (f.data.v[i] >= v->blocks) {
            affs_set_error(e, "Amiga file data pointer out of range");
            goto fail;
        }
        ld_bitmap_set(v->fixed_map, f.data.v[i], false);
    }
    if (fpush(&v->files, f)) goto oom;
    return 0;

oom:
    affs_set_error(e, "out of memory scanning Amiga filesystem");
fail:
    free_vec(&f.data);
    free_vec(&f.lists);
    return -1;
}

static int scan_dir(AffsVolume *v, uint32_t blk, bool root, uint8_t *seen,
                    unsigned depth, char **e) {
    if (depth > AFFS_MAX_DIRECTORY_DEPTH) {
        affs_set_error(e, "Amiga directory nesting exceeds supported depth");
        return -1;
    }
    if (blk >= v->blocks || ld_bitmap_get(seen, blk)) {
        affs_set_error(e, "Amiga directory graph contains a loop or invalid block %u", blk);
        return -1;
    }
    ld_bitmap_set(seen, blk, true);
    unsigned char b[BS];
    if (rd(v->fd, blk, b, e)) return -1;
    if (root) {
        if (!block_ok(b, T_SHORT, ST_ROOT, 5)) {
            affs_set_error(e, "invalid Amiga root block %u", blk);
            return -1;
        }
    } else if (!block_ok(b, T_SHORT, ST_DIR, 5) || lng(b, 1) != blk) {
        affs_set_error(e, "invalid Amiga directory block %u", blk);
        return -1;
    }
    ld_bitmap_set(v->fixed_map, blk, true);
    if (push(&v->directory_blocks, blk)) return -1;
    uint32_t hs = root ? lng(b, 3) : PTRS;
    if (hs > PTRS) hs = PTRS;
    for (uint32_t h = 0; h < hs; ++h) {
        uint32_t x = lng(b, 6 + (int)h);
        size_t chain = 0;
        while (x) {
            if (x >= v->blocks || ++chain > v->blocks) {
                affs_set_error(e, "invalid Amiga directory hash chain");
                return -1;
            }
            unsigned char eb[BS];
            if (rd(v->fd, x, eb, e) || lng(eb, 5) != csum(eb, 5) || lng(eb, 0) != T_SHORT) {
                affs_set_error(e, "invalid Amiga directory entry block %u", x);
                return -1;
            }
            ld_bitmap_set(v->fixed_map, x, true);
            uint32_t st = lng(eb, -1);
            uint32_t next = lng(eb, -4);
            if (st == ST_FILE) {
                if (scan_file(v, x, eb, e)) return -1;
            } else if (st == ST_DIR) {
                if (scan_dir(v, x, false, seen, depth + 1U, e)) return -1;
            } else {
                affs_set_error(e, "unsupported Amiga directory entry subtype 0x%08x at block %u",
                               st, x);
                return -1;
            }
            x = next;
        }
    }
    return 0;
}

static int read_bitmap(AffsVolume *v, const unsigned char root[BS], char **e) {
    for (int i = 0; i < 25; ++i) {
        uint32_t x = lng(root, -49 + i);
        if (!x) break;
        if (x >= v->blocks || push(&v->bitmap_blocks, x)) return -1;
        ld_bitmap_set(v->fixed_map, x, true);
    }
    uint32_t ext = lng(root, -24);
    size_t guard = 0;
    while (ext) {
        if (ext >= v->blocks || ++guard > v->blocks) {
            affs_set_error(e, "invalid Amiga bitmap extension chain");
            return -1;
        }
        unsigned char b[BS];
        if (rd(v->fd, ext, b, e)) return -1;
        if (push(&v->bitmap_ext_blocks, ext)) return -1;
        ld_bitmap_set(v->fixed_map, ext, true);
        for (int i = 0; i < 127; ++i) {
            uint32_t x = lng(b, i);
            if (!x) break;
            if (x >= v->blocks || push(&v->bitmap_blocks, x)) return -1;
            ld_bitmap_set(v->fixed_map, x, true);
        }
        ext = lng(b, -1);
    }
    size_t bits = (size_t)v->blocks - 2U;
    size_t longs = (bits + 31U) / 32U;
    size_t needed = (longs + 126U) / 127U;
    if (v->bitmap_blocks.n < needed) {
        affs_set_error(e, "Amiga free-space bitmap is incomplete");
        return -1;
    }
    size_t bit = 0;
    for (size_t bi = 0; bi < needed; ++bi) {
        unsigned char b[BS];
        if (rd(v->fd, v->bitmap_blocks.v[bi], b, e) || lng(b, 0) != csum(b, 0)) {
            affs_set_error(e, "invalid Amiga bitmap block %u", v->bitmap_blocks.v[bi]);
            return -1;
        }
        for (int li = 1; li < 128 && bit < bits; ++li) {
            uint32_t w = lng(b, li);
            for (unsigned k = 0; k < 32 && bit < bits; ++k, ++bit) {
                ld_bitmap_set(v->free_map, 2U + bit,
                              (w & (1U << k)) != 0U);
            }
        }
    }
    return 0;
}

int affs_scan(const char *path, bool writable, AffsVolume *v, char **e) {
    memset(v, 0, sizeof(*v));
    v->fd = -1;
    v->block_size = BS;
    v->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (v->fd < 0) {
        affs_set_error(e, "cannot open Amiga target %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(v->fd, &st)) {
        affs_set_error(e, "cannot stat Amiga target: %s", strerror(errno));
        goto fail;
    }
    off_t end = lseek(v->fd, 0, SEEK_END);
    if (end <= 0 || ((uint64_t)end % BS) != 0) {
        affs_set_error(e, "Amiga target size is not a positive multiple of 512 bytes");
        goto fail;
    }
    v->bytes = (uint64_t)end;
    v->blocks = (uint32_t)(v->bytes / BS);
    if ((uint64_t)v->blocks * BS != v->bytes || v->blocks < 20) {
        affs_set_error(e, "Amiga volume geometry is unsupported");
        goto fail;
    }
    unsigned char boot[BS];
    if (rd(v->fd, 0, boot, e)) goto fail;
    if (memcmp(boot, "DOS", 3) || boot[3] > 7) {
        affs_set_error(e, "target is not Amiga OFS/FFS DOS\\0..DOS\\7");
        goto fail;
    }
    if (boot[3] >= 6U) {
        affs_set_error(e,
            "Amiga DOS\\6/DOS\\7 long-name media is outside the validated classic OFS/FFS writer");
        goto fail;
    }
    v->dostype = boot[3];
    v->ffs = (v->dostype & 1U) != 0;
    v->root = v->blocks / 2U;
    v->free_map = ld_bitmap_calloc(v->blocks);
    v->fixed_map = ld_bitmap_calloc(v->blocks);
    if (!v->free_map || !v->fixed_map) {
        affs_set_error(e, "out of memory scanning Amiga filesystem");
        goto fail;
    }
    ld_bitmap_set(v->fixed_map, 0U, true);
    ld_bitmap_set(v->fixed_map, 1U, true);
    unsigned char root[BS];
    if (rd(v->fd, v->root, root, e) || !block_ok(root, T_SHORT, ST_ROOT, 5)) {
        affs_set_error(e, "invalid Amiga root block at %u", v->root);
        goto fail;
    }
    if (lng(root, -50) != UINT32_MAX) {
        affs_set_error(e,
            "Amiga root marks the allocation bitmap invalid; repair is required before Defragmenter may trust it");
        goto fail;
    }
    if (read_bitmap(v, root, e)) goto fail;
    uint8_t *seen = ld_bitmap_calloc(v->blocks);
    if (!seen) {
        affs_set_error(e, "out of memory scanning Amiga filesystem");
        goto fail;
    }
    int rc = scan_dir(v, v->root, true, seen, 0U, e);
    free(seen);
    if (rc) goto fail;
    for (uint32_t i = 0; i < v->blocks; ++i) {
        if (!ld_bitmap_get(v->free_map, i))
            ld_bitmap_set(v->fixed_map, i, true);
    }
    for (size_t f = 0; f < v->files.n; ++f) {
        for (size_t j = 0; j < v->files.v[f].data.n; ++j) {
            ld_bitmap_set(v->fixed_map, v->files.v[f].data.v[j], false);
        }
    }
    return 0;

fail:
    affs_close(v);
    return -1;
}

static void print_ranges(const uint8_t *map, uint32_t blocks, bool want) {
    bool first = true;
    printf("[");
    for (uint32_t i = 0; i < blocks;) {
        while (i < blocks && ld_bitmap_get(map, i) != want) ++i;
        if (i == blocks) break;
        uint32_t s = i;
        while (i < blocks && ld_bitmap_get(map, i) == want) ++i;
        if (!first) printf(",");
        printf("[%u,%u]", s, i);
        first = false;
    }
    printf("]");
}

int affs_analyse_json(const char *path, char **e) {
    AffsVolume v;
    if (affs_scan(path, false, &v, e)) return -1;
    uint64_t ff = 0;
    printf("{\"filesystem\":\"affs\",\"variant\":\"%s\",\"dostype\":%u,"
           "\"block_size\":512,\"total_blocks\":%u,\"regular_files\":%zu,"
           "\"directories\":%zu,\"fragmented_files\":",
           v.ffs ? "FFS" : "OFS", v.dostype, v.blocks, v.files.n, v.directory_blocks.n);
    size_t frag = 0;
    for (size_t i = 0; i < v.files.n; ++i) {
        if (affs_fragments(&v.files.v[i].data) > 1) {
            ++frag;
            ff += v.files.v[i].data.n;
        }
    }
    printf("%zu,\"fragmented_directories\":0,\"fragmented_blocks\":%" PRIu64
           ",\"free_ranges\":", frag, ff);
    print_ranges(v.free_map, v.blocks, true);
    printf(",\"fragmented_ranges\":[");
    bool first = true;
    for (size_t f = 0; f < v.files.n; ++f) {
        if (affs_fragments(&v.files.v[f].data) > 1) {
            for (size_t j = 0; j < v.files.v[f].data.n; ++j) {
                if (!first) printf(",");
                printf("[%u,%u]", v.files.v[f].data.v[j], v.files.v[f].data.v[j] + 1U);
                first = false;
            }
        }
    }
    printf("],\"directory_ranges\":[");
    first = true;
    for (size_t i = 0; i < v.directory_blocks.n; ++i) {
        if (!first) printf(",");
        printf("[%u,%u]", v.directory_blocks.v[i], v.directory_blocks.v[i] + 1U);
        first = false;
    }
    printf("]}\n");
    affs_close(&v);
    return 0;
}

static uint32_t contiguous_map_run(const uint8_t *free_map, uint32_t blocks,
                                   uint32_t start, bool want_free, uint32_t limit) {
    uint32_t count = 0U;
    while (start + count < blocks && count < limit &&
           (ld_bitmap_get(free_map, (uint64_t)start + count)) == want_free) {
        ++count;
    }
    return count;
}

static int copy_allocated(const AffsVolume *v, int out, char **e) {
    unsigned char *buffer = malloc(AFFS_IO_BATCH_BYTES);
    if (buffer == NULL) {
        affs_set_error(e, "out of memory batching Amiga working-image I/O");
        return -1;
    }
    int rc = 0;
    for (uint32_t i = 0; i < v->blocks;) {
        if (ld_bitmap_get(v->free_map, i)) {
            ++i;
            continue;
        }
        uint32_t count = contiguous_map_run(v->free_map, v->blocks, i, false,
                                            AFFS_IO_BATCH_BLOCKS);
        size_t bytes = (size_t)count * BS;
        uint64_t offset = (uint64_t)i * BS;
        if (pread_full_local(v->fd, buffer, bytes, offset) != (ssize_t)bytes ||
            pwrite_full_local(out, buffer, bytes, offset) != (ssize_t)bytes) {
            affs_set_error(e, "cannot build Amiga working image at blocks %u..%u: %s",
                           i, i + count, strerror(errno));
            rc = -1;
            break;
        }
        i += count;
    }
    free(buffer);
    return rc;
}

static int bitmap_write(AffsVolume *v, char **e) {
    size_t bits = (size_t)v->blocks - 2U;
    size_t bit = 0;
    for (size_t bi = 0; bi < v->bitmap_blocks.n && bit < bits; ++bi) {
        unsigned char b[BS];
        if (rd(v->fd, v->bitmap_blocks.v[bi], b, e)) return -1;
        for (int li = 1; li < 128 && bit < bits; ++li) {
            uint32_t w = 0;
            for (unsigned k = 0; k < 32 && bit < bits; ++k, ++bit) {
                if (ld_bitmap_get(v->free_map, 2U + bit)) w |= 1U << k;
            }
            plng(b, li, w);
        }
        fixsum(b, 0);
        if (wr(v->fd, v->bitmap_blocks.v[bi], b, e)) return -1;
    }
    return 0;
}

static int rewrite_ptrs(AffsVolume *v, AffsFile *f, const AffsU32Vec *dest, char **e) {
    unsigned char b[BS];
    if (rd(v->fd, f->header, b, e)) return -1;
    size_t off = 0;
    uint32_t n = (uint32_t)(dest->n < PTRS ? dest->n : PTRS);
    plng(b, 2, n);
    for (uint32_t i = 0; i < PTRS; ++i) plng(b, -51 - (int)i, i < n ? dest->v[i] : 0);
    plng(b, 4, n ? dest->v[0] : 0);
    fixsum(b, 5);
    if (wr(v->fd, f->header, b, e)) return -1;
    off = n;
    for (size_t li = 0; li < f->lists.n; ++li) {
        if (rd(v->fd, f->lists.v[li], b, e)) return -1;
        n = (uint32_t)((dest->n - off) < PTRS ? (dest->n - off) : PTRS);
        plng(b, 2, n);
        for (uint32_t i = 0; i < PTRS; ++i) {
            plng(b, -51 - (int)i, i < n ? dest->v[off + i] : 0);
        }
        fixsum(b, 5);
        if (wr(v->fd, f->lists.v[li], b, e)) return -1;
        off += n;
    }
    return off == dest->n ? 0 : -1;
}

static int choose_run(AffsVolume *v, uint32_t need, uint32_t reserve, uint32_t *start) {
    for (uint32_t s = 2; s + need + reserve <= v->blocks; ++s) {
        bool ok = true;
        for (uint32_t k = 0; k < need + reserve; ++k) {
            if (ld_bitmap_get(v->fixed_map, (uint64_t)s + k)) {
                ok = false;
                s += k;
                break;
            }
        }
        if (ok) {
            *start = s;
            for (uint32_t k = 0; k < need; ++k)
                ld_bitmap_set(v->fixed_map, (uint64_t)s + k, true);
            for (uint32_t k = need; k < need + reserve; ++k)
                ld_bitmap_set(v->fixed_map, (uint64_t)s + k, true);
            return 0;
        }
    }
    return -1;
}

static uint32_t contiguous_file_run(const AffsU32Vec *data, uint32_t index) {
    uint32_t count = 1U;
    while ((size_t)index + count < data->n && count < AFFS_IO_BATCH_BLOCKS &&
           data->v[index + count] == data->v[index] + count) {
        ++count;
    }
    return count;
}

static int move_file_data_batched(const AffsVolume *src, AffsVolume *dst,
                                  const AffsFile *sf, uint32_t start,
                                  uint64_t *changed, char **e) {
    unsigned char *buffer = malloc(AFFS_IO_BATCH_BYTES);
    if (buffer == NULL) {
        affs_set_error(e, "out of memory batching Amiga file relocation");
        return -1;
    }
    int rc = 0;
    uint32_t j = 0U;
    uint32_t need = (uint32_t)sf->data.n;
    while (j < need) {
        uint32_t count = contiguous_file_run(&sf->data, j);
        size_t bytes = (size_t)count * BS;
        uint64_t source_offset = (uint64_t)sf->data.v[j] * BS;
        uint64_t target_offset = (uint64_t)(start + j) * BS;
        if (pread_full_local(src->fd, buffer, bytes, source_offset) != (ssize_t)bytes) {
            affs_set_error(e, "cannot read Amiga file-data run at block %u: %s",
                           sf->data.v[j], strerror(errno));
            rc = -1;
            break;
        }
        if (!src->ffs) {
            for (uint32_t k = 0; k < count; ++k) {
                unsigned char *block = buffer + (size_t)k * BS;
                uint32_t logical = j + k;
                plng(block, 1, sf->header);
                plng(block, 2, logical + 1U);
                plng(block, 4, (logical + 1U < need) ? start + logical + 1U : 0);
                fixsum(block, 5);
            }
        }
        if (pwrite_full_local(dst->fd, buffer, bytes, target_offset) != (ssize_t)bytes) {
            affs_set_error(e, "cannot write Amiga file-data run at block %u: %s",
                           start + j, strerror(errno));
            rc = -1;
            break;
        }
        for (uint32_t block = 0U; block < count; ++block)
            ld_bitmap_set(dst->free_map, (uint64_t)start + j + block, false);
        *changed += count;
        j += count;
    }
    free(buffer);
    return rc;
}

int affs_build_stage(const char *source, const char *stage, bool growth, unsigned gp,
                     bool live, uint64_t *commit_bytes, char **e) {
    AffsVolume src;
    if (affs_scan(source, false, &src, e)) return -1;
    (void)unlink(stage);
    int stage_flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    stage_flags |= O_NOFOLLOW;
#endif
    int out = open(stage, stage_flags, 0600);
    if (out < 0) {
        affs_set_error(e, "cannot create Amiga working image: %s", strerror(errno));
        affs_close(&src);
        return -1;
    }
    printf("Amiga preparation batching: up to %u KiB per contiguous I/O batch.\n",
           (unsigned)(AFFS_IO_BATCH_BYTES / 1024U));
    fflush(stdout);
    if (ftruncate(out, (off_t)src.bytes) || copy_allocated(&src, out, e)) {
        close(out);
        affs_close(&src);
        return -1;
    }
    close(out);

    AffsVolume dst;
    if (affs_scan(stage, true, &dst, e)) {
        affs_close(&src);
        return -1;
    }
    for (size_t f = 0; f < src.files.n; ++f) {
        for (size_t j = 0; j < src.files.v[f].data.n; ++j) {
            ld_bitmap_set(dst.free_map, src.files.v[f].data.v[j], true);
            ld_bitmap_set(dst.fixed_map, src.files.v[f].data.v[j], false);
        }
    }

    uint64_t changed = 0;
    for (size_t fi = 0; fi < src.files.n; ++fi) {
        AffsFile *sf = &src.files.v[fi];
        AffsFile *df = &dst.files.v[fi];
        uint32_t need = (uint32_t)sf->data.n;
        uint32_t reserve = growth && need ? ((need * gp + 99U) / 100U) : 0;
        uint32_t start = 0;
        if (need && choose_run(&dst, need, reserve, &start)) {
            affs_set_error(e, "Amiga layout cannot place file header %u contiguously with required reserve",
                           sf->header);
            goto fail;
        }
        AffsU32Vec nv = {0};
        for (uint32_t j = 0; j < need; ++j) {
            if (push(&nv, start + j)) {
                affs_set_error(e, "out of memory planning Amiga layout");
                free_vec(&nv);
                goto fail;
            }
        }
        if (need && move_file_data_batched(&src, &dst, sf, start, &changed, e) != 0) {
            free_vec(&nv);
            goto fail;
        }
        for (uint32_t j = 0; j < reserve; ++j) ld_bitmap_set(dst.free_map, (uint64_t)start + need + j, true);
        if (rewrite_ptrs(&dst, df, &nv, e)) {
            free_vec(&nv);
            goto fail;
        }
        if (live && need) {
            printf("@@LIVE_RANGES {\"ranges\":[[%u,%u,1]],\"sequence\":%zu}\n",
                   start, start + need, fi + 1);
            fflush(stdout);
        }
        free_vec(&nv);
    }

    if (bitmap_write(&dst, e) || fsync(dst.fd)) goto fail;
    if (commit_bytes) {
        uint64_t c = 0;
        for (uint32_t i = 0; i < dst.blocks; ++i) {
            if (!ld_bitmap_get(dst.free_map, i)) c += BS;
        }
        *commit_bytes = c;
    }
    printf("Amiga native C layout: relocated %" PRIu64 " data blocks; metadata remained fixed.\n",
           changed);
    affs_close(&dst);
    affs_close(&src);
    return 0;

fail:
    affs_close(&dst);
    affs_close(&src);
    return -1;
}

int affs_verify_layout(const char *path, bool growth, unsigned gp, char **e) {
    AffsVolume v;
    if (affs_scan(path, false, &v, e)) return -1;
    for (size_t i = 0; i < v.files.n; ++i) {
        AffsFile *f = &v.files.v[i];
        if (affs_fragments(&f->data) > 1) {
            affs_set_error(e, "Amiga file header %u remains fragmented", f->header);
            affs_close(&v);
            return -1;
        }
        if (growth && f->data.n) {
            uint32_t reserve = ((uint32_t)f->data.n * gp + 99U) / 100U;
            uint32_t end = f->data.v[f->data.n - 1] + 1U;
            for (uint32_t k = 0; k < reserve; ++k) {
                if (end + k >= v.blocks || !ld_bitmap_get(v.free_map, (uint64_t)end + k)) {
                    affs_set_error(e, "Amiga file header %u does not have its required growth reserve",
                                   f->header);
                    affs_close(&v);
                    return -1;
                }
            }
        }
    }
    affs_close(&v);
    return 0;
}

int affs_commit_stage(const char *stage, const char *target, uint64_t *written, char **e) {
    AffsVolume v;
    if (affs_scan(stage, false, &v, e)) return -1;
    int out = ld_device_open_verified_fd(target, true, NULL, 0U);
    if (out < 0) {
        affs_set_error(e, "cannot open Amiga source for commit: %s", strerror(errno));
        affs_close(&v);
        return -1;
    }
    unsigned char *buffer = malloc(AFFS_IO_BATCH_BYTES);
    if (buffer == NULL) {
        affs_set_error(e, "out of memory batching Amiga commit I/O");
        close(out);
        affs_close(&v);
        return -1;
    }
    uint64_t n = 0;
    int rc = 0;
    for (uint32_t i = 0; i < v.blocks;) {
        if (ld_bitmap_get(v.free_map, i)) {
            ++i;
            continue;
        }
        uint32_t count = contiguous_map_run(v.free_map, v.blocks, i, false,
                                            AFFS_IO_BATCH_BLOCKS);
        size_t bytes = (size_t)count * BS;
        uint64_t offset = (uint64_t)i * BS;
        if (pread_full_local(v.fd, buffer, bytes, offset) != (ssize_t)bytes ||
            pwrite_full_local(out, buffer, bytes, offset) != (ssize_t)bytes) {
            affs_set_error(e, "cannot commit Amiga blocks %u..%u: %s",
                           i, i + count, strerror(errno));
            rc = -1;
            break;
        }
        n += bytes;
        i += count;
    }
    if (rc == 0 && fsync(out)) {
        affs_set_error(e, "cannot sync Amiga source: %s", strerror(errno));
        rc = -1;
    }
    free(buffer);
    close(out);
    affs_close(&v);
    if (rc == 0 && written) *written = n;
    return rc;
}
