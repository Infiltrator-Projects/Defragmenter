// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfs_native.h"

#include "ld_device.h"
#include "ld_io.h"

#include "infiltratr/arithmetic.h"
#include "infiltratr/endian.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define SFS0_ROOT_ID 0x53465300U
#define SFS2_ROOT_ID 0x53465302U
#define SFS_BITMAP_ID 0x42544d50U
#define SFS_TRFA_ID 0x54524641U
#define SFS_BNODE_ID 0x424e4443U
#define SFS_OBJECT_ID 0x4f424a43U
#define SFS0_STRUCTURE_VERSION 3U
#define SFS2_STRUCTURE_VERSION 4U
#define SFS_BNODE_HEADER_BYTES 16U
#define SFS_INTERNAL_NODE_BYTES 8U
#define SFS0_EXTENT_NODE_BYTES 14U
#define SFS2_EXTENT_NODE_BYTES 16U
#define SFS_OBJECT_CONTAINER_BYTES 24U
#define SFS0_OBJECT_FIXED_BYTES 25U
#define SFS2_OBJECT_FIXED_BYTES 27U
#define SFS_OTYPE_HARDLINK 32U
#define SFS_OTYPE_LINK 64U
#define SFS_OTYPE_DIR 128U
#define SFS_MIN_BLOCK 512U
#define SFS_MAX_BLOCK 65536U
#define SFS_HEADER_BYTES 12U
#define SFS_MAX_FILENAME 107U
#define SFS_IO_BATCH_BYTES (1024U * 1024U)

static void set_error(char *error, size_t size, const char *text) {
    if (error != NULL && size != 0U) (void)snprintf(error, size, "%s", text);
}
static int size_bytes(int fd, const char *path, uint64_t *bytes) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0) { errno = EINVAL; return -1; }
        *bytes = (uint64_t)st.st_size;
        return 0;
    }
    if (!S_ISBLK(st.st_mode)) { (void)path; errno = EINVAL; return -1; }
    return ioctl(fd, BLKGETSIZE64, bytes) == 0 ? 0 : -1;
}
static bool valid_block_size(uint32_t value) {
    return value >= SFS_MIN_BLOCK && value <= SFS_MAX_BLOCK &&
           (value & (value - 1U)) == 0U && value % 512U == 0U;
}
static uint32_t checksum_seed_for_root_id(uint32_t root_id) {
    return root_id == SFS2_ROOT_ID ? 2U : 1U;
}

static bool checksum_ok(const uint8_t *block, uint32_t block_size,
                        uint32_t seed) {
    if (block_size % 4U != 0U) return false;
    uint32_t sum = seed;
    for (uint32_t off = 0U; off < block_size; off += 4U)
        sum += infiltratr_load_be32(block + off);
    return sum == 0U;
}
typedef struct {
    uint32_t block_size, total_blocks, bitmap_base, root_object_container;
    uint32_t admin_space_container, extent_bnode_root, object_node_root;
    uint32_t root_id;
    uint16_t version, sequence;
    uint8_t bits;
    uint32_t own_block;
} Root;

static bool root_id_supported(uint32_t root_id) {
    return root_id == SFS0_ROOT_ID || root_id == SFS2_ROOT_ID;
}

static bool root_is_sfs2(const Root *root) {
    return root != NULL && root->root_id == SFS2_ROOT_ID &&
           root->version == SFS2_STRUCTURE_VERSION;
}

static uint32_t checksum_seed(const Root *root) {
    return root_is_sfs2(root) ? 2U : 1U;
}

static uint8_t extent_node_bytes(const Root *root) {
    return root_is_sfs2(root) ? SFS2_EXTENT_NODE_BYTES : SFS0_EXTENT_NODE_BYTES;
}

static uint8_t object_fixed_bytes(const Root *root) {
    return root_is_sfs2(root) ? SFS2_OBJECT_FIXED_BYTES : SFS0_OBJECT_FIXED_BYTES;
}

typedef struct {
    uint32_t key;
    uint32_t next;
    uint32_t prev;
    uint32_t blocks;
    uint32_t container_block;
    uint32_t node_offset;
} SfsExtent;

typedef struct {
    SfsExtent *items;
    size_t count;
    size_t capacity;
} SfsExtentVec;

typedef struct {
    size_t *items;
    size_t count;
    size_t capacity;
} SfsSizeVec;

typedef struct {
    uint32_t *items;
    size_t count;
    size_t capacity;
} SfsU32Vec;

typedef struct {
    uint32_t object_block;
    uint32_t object_offset;
    uint32_t object_node;
    uint64_t byte_size;
    uint32_t first_extent;
    uint32_t new_start;
    SfsSizeVec chain;
} SfsFile;

typedef struct {
    SfsFile *items;
    size_t count;
    size_t capacity;
} SfsFileVec;

typedef struct {
    int fd;
    uint64_t physical_bytes;
    Root root;
    uint8_t *free_map;
    SfsExtentVec extents;
    SfsFileVec files;
    bool transaction_pending;
} SfsModel;

static int reserve_array(void **items, size_t *capacity, size_t need,
                         size_t item_size, char *error, size_t error_size) {
    if (infiltratr_array_reserve(items, capacity, item_size, need, 16U))
        return 0;
    set_error(error, error_size, "cannot grow SFS catalogue");
    return -1;
}

static int extent_push(SfsExtentVec *vec, SfsExtent value,
                       char *error, size_t error_size) {
    if (reserve_array((void **)&vec->items, &vec->capacity, vec->count + 1U,
                      sizeof(*vec->items), error, error_size) != 0) return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static int size_push(SfsSizeVec *vec, size_t value,
                     char *error, size_t error_size) {
    if (reserve_array((void **)&vec->items, &vec->capacity, vec->count + 1U,
                      sizeof(*vec->items), error, error_size) != 0) return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static int u32_push(SfsU32Vec *vec, uint32_t value,
                    char *error, size_t error_size) {
    if (reserve_array((void **)&vec->items, &vec->capacity, vec->count + 1U,
                      sizeof(*vec->items), error, error_size) != 0) return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static int file_push(SfsFileVec *vec, SfsFile value,
                     char *error, size_t error_size) {
    if (reserve_array((void **)&vec->items, &vec->capacity, vec->count + 1U,
                      sizeof(*vec->items), error, error_size) != 0) return -1;
    vec->items[vec->count++] = value;
    return 0;
}

static void files_free(SfsFileVec *files) {
    for (size_t index = 0U; index < files->count; ++index)
        free(files->items[index].chain.items);
    free(files->items);
    memset(files, 0, sizeof(*files));
}

static bool parse_root(const uint8_t *block, uint32_t bytes, uint32_t expected_own,
                       Root *root) {
    const uint32_t root_id = infiltratr_load_be32(block);
    if (bytes < 116U || !root_id_supported(root_id) ||
        infiltratr_load_be32(block + 8U) != expected_own ||
        !checksum_ok(block, bytes, checksum_seed_for_root_id(root_id)))
        return false;
    Root r = {0};
    r.own_block = expected_own;
    r.root_id = root_id;
    r.version = infiltratr_load_be16(block + 12U);
    r.sequence = infiltratr_load_be16(block + 14U);
    r.bits = block[20U];
    r.total_blocks = infiltratr_load_be32(block + 48U);
    r.block_size = infiltratr_load_be32(block + 52U);
    r.bitmap_base = infiltratr_load_be32(block + 96U);
    r.admin_space_container = infiltratr_load_be32(block + 100U);
    r.root_object_container = infiltratr_load_be32(block + 104U);
    r.extent_bnode_root = infiltratr_load_be32(block + 108U);
    r.object_node_root = infiltratr_load_be32(block + 112U);
    const bool version_matches =
        (r.root_id == SFS0_ROOT_ID && r.version == SFS0_STRUCTURE_VERSION) ||
        (r.root_id == SFS2_ROOT_ID && r.version == SFS2_STRUCTURE_VERSION);
    if (!version_matches || !valid_block_size(r.block_size) ||
        r.block_size != bytes || r.total_blocks < 4U || r.bitmap_base == 0U ||
        r.bitmap_base >= r.total_blocks || r.root_object_container >= r.total_blocks ||
        r.admin_space_container >= r.total_blocks || r.extent_bnode_root >= r.total_blocks ||
        r.object_node_root >= r.total_blocks) return false;
    *root = r;
    return true;
}
static int load_root_at(int fd, uint32_t block_size, uint32_t block_no, Root *root) {
    uint8_t *buf = malloc(block_size);
    if (buf == NULL) return -1;
    const ssize_t rr = ld_pread_full(fd, buf, block_size, (uint64_t)block_no * block_size);
    const bool ok = rr == (ssize_t)block_size && parse_root(buf, block_size, block_no, root);
    free(buf);
    return ok ? 0 : 1;
}
static int discover_roots(int fd, uint64_t physical, Root *selected,
                          bool *primary_valid, bool *backup_valid,
                          char *error, size_t error_size) {
    uint8_t probe[512];
    if (ld_pread_full(fd, probe, sizeof(probe), 0U) != (ssize_t)sizeof(probe)) {
        set_error(error, error_size, "SFS volume is shorter than its root block"); return -1;
    }
    Root primary = {0}, backup = {0};
    bool pvalid = false, bvalid = false;
    uint32_t bs = infiltratr_load_be32(probe + 52U);
    uint32_t total = infiltratr_load_be32(probe + 48U);
    if (root_id_supported(infiltratr_load_be32(probe)) &&
        valid_block_size(bs) && total >= 4U &&
        (uint64_t)total * bs <= physical) {
        pvalid = load_root_at(fd, bs, 0U, &primary) == 0;
        bvalid = load_root_at(fd, bs, total - 1U, &backup) == 0;
    }
    if (!pvalid && !bvalid) {
        for (uint32_t candidate = SFS_MIN_BLOCK; candidate <= SFS_MAX_BLOCK; candidate <<= 1U) {
            if (physical < candidate * 4ULL || physical % candidate != 0U) continue;
            const uint64_t blocks64 = physical / candidate;
            if (blocks64 > UINT32_MAX) continue;
            Root recovered = {0};
            if (load_root_at(fd, candidate, (uint32_t)blocks64 - 1U, &recovered) == 0 &&
                recovered.total_blocks == blocks64) {
                backup = recovered; bvalid = true; bs = candidate; total = (uint32_t)blocks64; break;
            }
        }
    }
    if (!pvalid && !bvalid) {
        set_error(error, error_size, "no valid SFS root block found"); return -1;
    }
    if (pvalid && (uint64_t)primary.total_blocks * primary.block_size > physical) pvalid = false;
    if (bvalid && (uint64_t)backup.total_blocks * backup.block_size > physical) bvalid = false;
    if (!pvalid && !bvalid) { set_error(error, error_size, "SFS root geometry exceeds target"); return -1; }
    Root chosen = pvalid ? primary : backup;
    if (bvalid && (!pvalid || backup.sequence > primary.sequence)) chosen = backup;
    if (pvalid && bvalid && (primary.block_size != backup.block_size ||
        primary.total_blocks != backup.total_blocks ||
        primary.root_id != backup.root_id ||
        primary.version != backup.version)) {
        set_error(error, error_size, "SFS redundant roots disagree on filesystem geometry"); return -1;
    }
    *selected = chosen; *primary_valid = pvalid; *backup_valid = bvalid;
    return 0;
}
static int validate_transaction(int fd, const Root *root, bool *pending,
                                char *error, size_t error_size) {
    *pending = false;
    if (root->root_object_container > root->total_blocks - 3U) return 0;
    const uint32_t block_no = root->root_object_container + 2U;
    uint8_t *buf = malloc(root->block_size);
    if (buf == NULL) { set_error(error, error_size, "out of memory reading SFS transaction state"); return -1; }
    const ssize_t rr = ld_pread_full(fd, buf, root->block_size, (uint64_t)block_no * root->block_size);
    if (rr != (ssize_t)root->block_size) { free(buf); set_error(error, error_size, "cannot read SFS transaction state"); return -1; }
    if (infiltratr_load_be32(buf) == SFS_TRFA_ID) {
        if (infiltratr_load_be32(buf + 8U) != block_no || !checksum_ok(buf, root->block_size, checksum_seed(root))) {
            free(buf); set_error(error, error_size, "corrupt SFS unfinished-transaction marker"); return -1;
        }
        *pending = true;
    }
    free(buf); return 0;
}

static int read_metadata_block(int fd, const Root *root, uint32_t block_no,
                               uint32_t expected_id, uint8_t *buffer,
                               char *error, size_t error_size) {
    if (block_no >= root->total_blocks ||
        ld_pread_full(fd, buffer, root->block_size,
                      (uint64_t)block_no * root->block_size) !=
            (ssize_t)root->block_size) {
        set_error(error, error_size, "cannot read SFS metadata block");
        return -1;
    }
    if (infiltratr_load_be32(buffer) != expected_id || infiltratr_load_be32(buffer + 8U) != block_no ||
        !checksum_ok(buffer, root->block_size, checksum_seed(root))) {
        set_error(error, error_size, "invalid SFS metadata block");
        return -1;
    }
    return 0;
}

static int scan_extent_container(int fd, const Root *root, uint32_t block_no,
                                 uint8_t *visited, unsigned depth,
                                 SfsExtentVec *extents,
                                 uint32_t *first_key, bool *has_key,
                                 char *error, size_t error_size) {
    if (depth > 64U || block_no >= root->total_blocks || visited[block_no] != 0U) {
        set_error(error, error_size, "SFS extent B-tree contains a cycle or invalid depth");
        return -1;
    }
    visited[block_no] = 1U;
    uint8_t *buffer = malloc(root->block_size);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory reading SFS extent B-tree");
        return -1;
    }
    if (read_metadata_block(fd, root, block_no, SFS_BNODE_ID, buffer,
                            error, error_size) != 0) {
        free(buffer);
        return -1;
    }

    const uint16_t count = infiltratr_load_be16(buffer + 12U);
    const bool leaf = buffer[14U] != 0U;
    const uint8_t node_size = buffer[15U];
    const uint8_t expected_size =
        leaf ? extent_node_bytes(root) : SFS_INTERNAL_NODE_BYTES;
    if (node_size != expected_size ||
        (uint64_t)count * node_size > root->block_size - SFS_BNODE_HEADER_BYTES) {
        free(buffer);
        set_error(error, error_size, "invalid SFS extent B-tree node geometry");
        return -1;
    }

    *has_key = false;
    uint32_t previous_key = 0U;
    for (uint16_t index = 0U; index < count; ++index) {
        const uint8_t *node = buffer + SFS_BNODE_HEADER_BYTES +
                              (size_t)index * node_size;
        const uint32_t key = infiltratr_load_be32(node);
        if (index != 0U && key <= previous_key) {
            free(buffer);
            set_error(error, error_size, "SFS extent B-tree keys are not strictly ordered");
            return -1;
        }
        previous_key = key;
        if (leaf) {
            SfsExtent extent = {
                .key = key,
                .next = infiltratr_load_be32(node + 4U),
                .prev = infiltratr_load_be32(node + 8U),
                .blocks = root_is_sfs2(root)
                    ? infiltratr_load_be32(node + 12U)
                    : infiltratr_load_be16(node + 12U),
                .container_block = block_no,
                .node_offset = SFS_BNODE_HEADER_BYTES + (uint32_t)index * node_size,
            };
            if (extent.key == 0U || extent.blocks == 0U ||
                extent.key >= root->total_blocks ||
                (uint64_t)extent.key + extent.blocks > root->total_blocks ||
                extent.next >= root->total_blocks ||
                extent.prev >= root->total_blocks) {
                free(buffer);
                set_error(error, error_size, "SFS extent B-tree contains an invalid extent");
                return -1;
            }
            if (extent_push(extents, extent, error, error_size) != 0) {
                free(buffer);
                return -1;
            }
            if (!*has_key) {
                *first_key = key;
                *has_key = true;
            }
        } else {
            const uint32_t child = infiltratr_load_be32(node + 4U);
            uint32_t child_first = 0U;
            bool child_has_key = false;
            if (child == 0U || child >= root->total_blocks ||
                scan_extent_container(fd, root, child, visited, depth + 1U,
                                      extents, &child_first, &child_has_key,
                                      error, error_size) != 0) {
                free(buffer);
                if (error != NULL && error_size != 0U && error[0] == '\0')
                    set_error(error, error_size, "SFS extent B-tree has an invalid child");
                return -1;
            }
            if (!child_has_key || (index == 0U ? key > child_first : key != child_first)) {
                free(buffer);
                set_error(error, error_size, "SFS extent B-tree separator key is inconsistent");
                return -1;
            }
            if (!*has_key) {
                *first_key = child_first;
                *has_key = true;
            }
        }
    }
    free(buffer);
    return 0;
}

static ssize_t extent_find(const SfsExtentVec *extents, uint32_t key) {
    size_t lo = 0U;
    size_t hi = extents->count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        if (extents->items[mid].key < key) lo = mid + 1U;
        else hi = mid;
    }
    if (lo < extents->count && extents->items[lo].key == key)
        return (ssize_t)lo;
    return -1;
}

static void add_fragmented_cells(SfsMapCell *cells, uint64_t cell_count,
                                 uint32_t start, uint32_t blocks) {
    if (cells == NULL || cell_count == 0U || blocks == 0U) return;
    const uint64_t end = (uint64_t)start + blocks;
    for (uint64_t index = 0U; index < cell_count; ++index) {
        const uint64_t cell_start = cells[index].start;
        const uint64_t cell_end = cells[index].end + 1U;
        const uint64_t overlap_start = cell_start > start ? cell_start : start;
        const uint64_t overlap_end = cell_end < end ? cell_end : end;
        if (overlap_end > overlap_start)
            cells[index].fragmented_count += overlap_end - overlap_start;
    }
}

static int evaluate_file(const Root *root, const SfsExtentVec *extents,
                         uint8_t *extent_owned, const uint8_t *free_map,
                         uint32_t first, uint64_t size,
                         SfsAnalysis *analysis, SfsMapCell *cells,
                         uint64_t cell_count, SfsSizeVec *record_chain,
                         char *error, size_t error_size) {
    const uint64_t expected =
        ((uint64_t)size + root->block_size - 1U) / root->block_size;
    analysis->regular_files++;
    if (expected == 0U) {
        if (first != 0U) {
            set_error(error, error_size, "empty SFS file unexpectedly references data");
            return -1;
        }
        return 0;
    }
    if (first == 0U) {
        set_error(error, error_size, "non-empty SFS file has no first extent");
        return -1;
    }

    SfsSizeVec chain = {0};
    uint32_t key = first;
    uint32_t previous_key = 0U;
    uint32_t previous_blocks = 0U;
    uint64_t blocks = 0U;
    bool fragmented = false;
    while (key != 0U) {
        const ssize_t found = extent_find(extents, key);
        if (found < 0 || extent_owned[(size_t)found] != 0U) {
            free(chain.items);
            set_error(error, error_size, "SFS file extent chain is missing, cyclic or multiply referenced");
            return -1;
        }
        const SfsExtent *extent = &extents->items[(size_t)found];
        if (extent->prev != previous_key) {
            free(chain.items);
            set_error(error, error_size, "SFS file extent backward link is inconsistent");
            return -1;
        }
        if (previous_key != 0U &&
            extent->key != previous_key + (uint32_t)previous_blocks)
            fragmented = true;
        extent_owned[(size_t)found] = 1U;
        if (size_push(&chain, (size_t)found, error, error_size) != 0) {
            free(chain.items);
            return -1;
        }
        blocks += extent->blocks;
        if (blocks > expected) {
            free(chain.items);
            set_error(error, error_size, "SFS file extent chain exceeds file size");
            return -1;
        }
        previous_key = extent->key;
        previous_blocks = extent->blocks;
        key = extent->next;
    }
    if (blocks != expected) {
        free(chain.items);
        set_error(error, error_size, "SFS file extent chain does not cover file size");
        return -1;
    }

    analysis->data_blocks += blocks;
    if (fragmented) {
        analysis->fragmented_files++;
        for (size_t i = 0U; i < chain.count; ++i) {
            const SfsExtent *extent = &extents->items[chain.items[i]];
            add_fragmented_cells(cells, cell_count, extent->key, extent->blocks);
        }
    }

    const uint32_t end = previous_key + (uint32_t)previous_blocks;
    const uint64_t reserve = (blocks * 10U + 99U) / 100U;
    if (fragmented || (uint64_t)end + reserve > root->total_blocks) {
        analysis->growth_10_satisfied = false;
    } else {
        for (uint64_t i = 0U; i < reserve; ++i) {
            if (free_map[end + i] == 0U) {
                analysis->growth_10_satisfied = false;
                break;
            }
        }
    }
    if (record_chain != NULL) {
        *record_chain = chain;
    } else {
        free(chain.items);
    }
    return 0;
}

static bool object_name_valid(const uint8_t *name, size_t length) {
    if (name == NULL || length == 0U || length > SFS_MAX_FILENAME)
        return false;
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t character = name[index];
        if (character < UINT8_C(0x20) || character == (uint8_t)':' ||
            (character > UINT8_C(0x7e) && character < UINT8_C(0xa0)))
            return false;
    }
    return true;
}

static int scan_object_catalogue(int fd, const Root *root,
                                 const SfsExtentVec *extents,
                                 uint8_t *extent_owned, const uint8_t *free_map,
                                 SfsAnalysis *analysis, SfsMapCell *cells,
                                 uint64_t cell_count, SfsFileVec *files,
                                 char *error, size_t error_size) {
    uint8_t *visited = calloc(root->total_blocks, 1U);
    if (visited == NULL) {
        set_error(error, error_size, "out of memory tracking SFS object containers");
        return -1;
    }
    SfsU32Vec pending = {0};
    if (u32_push(&pending, root->root_object_container, error, error_size) != 0) {
        free(visited);
        return -1;
    }

    int rc = 0;
    for (size_t queue = 0U; queue < pending.count && rc == 0; ++queue) {
        uint32_t block_no = pending.items[queue];
        uint32_t expected_previous = 0U;
        while (block_no != 0U) {
            if (block_no >= root->total_blocks || visited[block_no] != 0U) {
                set_error(error, error_size, "SFS object-container graph contains a cycle");
                rc = -1;
                break;
            }
            visited[block_no] = 1U;
            uint8_t *buffer = malloc(root->block_size);
            if (buffer == NULL) {
                set_error(error, error_size, "out of memory reading SFS object container");
                rc = -1;
                break;
            }
            if (read_metadata_block(fd, root, block_no, SFS_OBJECT_ID, buffer,
                                    error, error_size) != 0) {
                free(buffer);
                rc = -1;
                break;
            }
            const uint32_t next_block = infiltratr_load_be32(buffer + 16U);
            const uint32_t previous_block = infiltratr_load_be32(buffer + 20U);
            if (previous_block != expected_previous ||
                next_block >= root->total_blocks) {
                free(buffer);
                set_error(error, error_size, "SFS object-container links are inconsistent");
                rc = -1;
                break;
            }

            const size_t fixed_bytes = object_fixed_bytes(root);
            size_t offset = SFS_OBJECT_CONTAINER_BYTES;
            while (offset + fixed_bytes + 2U <= root->block_size) {
                const size_t name_offset = offset + fixed_bytes;
                if (buffer[name_offset] == 0U) break;
                const uint8_t *limit = buffer + root->block_size;
                const uint8_t *name_end =
                    memchr(buffer + name_offset, 0, (size_t)(limit - (buffer + name_offset)));
                if (name_end == NULL) {
                    free(buffer);
                    set_error(error, error_size, "unterminated SFS object name");
                    rc = -1;
                    break;
                }
                const size_t name_length =
                    (size_t)(name_end - (buffer + name_offset));
                if (!object_name_valid(buffer + name_offset, name_length)) {
                    free(buffer);
                    set_error(error, error_size,
                              "SFS object name is invalid or exceeds 107 characters");
                    rc = -1;
                    break;
                }
                const uint8_t *comment = name_end + 1U;
                const uint8_t *comment_end =
                    memchr(comment, 0, (size_t)(limit - comment));
                if (comment_end == NULL) {
                    free(buffer);
                    set_error(error, error_size, "unterminated SFS object comment");
                    rc = -1;
                    break;
                }
                const uint32_t object_node = infiltratr_load_be32(buffer + offset + 4U);
                const uint32_t data = infiltratr_load_be32(buffer + offset + 12U);
                const uint32_t auxiliary = infiltratr_load_be32(buffer + offset + 16U);
                const uint64_t file_size = root_is_sfs2(root)
                    ? ((uint64_t)auxiliary << 16U) |
                      infiltratr_load_be16(buffer + offset + 20U)
                    : auxiliary;
                const uint8_t bits =
                    buffer[offset + (root_is_sfs2(root) ? 26U : 24U)];
                if (object_node == 0U) {
                    free(buffer);
                    set_error(error, error_size, "SFS object has an invalid node number");
                    rc = -1;
                    break;
                }

                if ((bits & SFS_OTYPE_DIR) != 0U) {
                    analysis->directories++;
                    if (auxiliary != 0U &&
                        u32_push(&pending, auxiliary, error, error_size) != 0) {
                        free(buffer);
                        rc = -1;
                        break;
                    }
                } else if ((bits & (SFS_OTYPE_LINK | SFS_OTYPE_HARDLINK)) == 0U) {
                    SfsSizeVec chain = {0};
                    if (evaluate_file(root, extents, extent_owned, free_map,
                                      data, file_size, analysis, cells, cell_count,
                                      files != NULL ? &chain : NULL,
                                      error, error_size) != 0) {
                        free(buffer);
                        rc = -1;
                        break;
                    }
                    if (files != NULL) {
                        SfsFile file = {
                            .object_block = block_no,
                            .object_offset = (uint32_t)offset,
                            .object_node = object_node,
                            .byte_size = file_size,
                            .first_extent = data,
                            .new_start = 0U,
                            .chain = chain,
                        };
                        if (file_push(files, file, error, error_size) != 0) {
                            free(chain.items);
                            free(buffer);
                            rc = -1;
                            break;
                        }
                    }
                }

                size_t next_offset = (size_t)(comment_end - buffer) + 1U;
                if ((next_offset & 1U) != 0U) ++next_offset;
                if (next_offset <= offset || next_offset > root->block_size) {
                    free(buffer);
                    set_error(error, error_size, "invalid SFS object-container packing");
                    rc = -1;
                    break;
                }
                offset = next_offset;
            }
            if (rc != 0) break;
            free(buffer);
            expected_previous = block_no;
            block_no = next_block;
        }
    }

    free(pending.items);
    free(visited);
    return rc;
}

static int scan_catalogue(int fd, const Root *root, const uint8_t *free_map,
                          SfsAnalysis *analysis, SfsMapCell *cells,
                          uint64_t cell_count, char *error, size_t error_size) {
    uint8_t *tree_visited = calloc(root->total_blocks, 1U);
    if (tree_visited == NULL) {
        set_error(error, error_size, "out of memory tracking SFS extent B-tree");
        return -1;
    }
    SfsExtentVec extents = {0};
    uint32_t first_key = 0U;
    bool has_key = false;
    int rc = scan_extent_container(fd, root, root->extent_bnode_root,
                                   tree_visited, 0U, &extents,
                                   &first_key, &has_key, error, error_size);
    free(tree_visited);
    if (rc != 0) {
        free(extents.items);
        return -1;
    }

    for (size_t index = 0U; index < extents.count; ++index) {
        const SfsExtent *extent = &extents.items[index];
        if (index != 0U) {
            const SfsExtent *previous = &extents.items[index - 1U];
            if ((uint64_t)previous->key + previous->blocks > extent->key) {
                free(extents.items);
                set_error(error, error_size, "SFS data extents overlap");
                return -1;
            }
        }
        for (uint32_t block = 0U; block < extent->blocks; ++block) {
            if (free_map[extent->key + block] != 0U) {
                free(extents.items);
                set_error(error, error_size, "SFS extent B-tree references bitmap-free data");
                return -1;
            }
        }
    }

    uint8_t *owned = calloc(extents.count == 0U ? 1U : extents.count, 1U);
    if (owned == NULL) {
        free(extents.items);
        set_error(error, error_size, "out of memory tracking SFS extent ownership");
        return -1;
    }
    analysis->growth_10_satisfied = true;
    rc = scan_object_catalogue(fd, root, &extents, owned, free_map,
                               analysis, cells, cell_count, NULL,
                               error, error_size);
    if (rc == 0) {
        for (size_t index = 0U; index < extents.count; ++index) {
            if (owned[index] == 0U) {
                set_error(error, error_size, "SFS extent B-tree contains unreferenced data");
                rc = -1;
                break;
            }
        }
    }
    free(owned);
    free(extents.items);
    (void)first_key;
    (void)has_key;
    return rc;
}

static int scan_bitmap(int fd, const Root *root, SfsAnalysis *analysis,
                       SfsMapCell *cells, uint64_t cell_count, uint64_t total_units,
                       uint8_t *free_map, char *error, size_t error_size) {
    const uint64_t bits_per = (uint64_t)(root->block_size - SFS_HEADER_BYTES) * 8U;
    const uint64_t bitmap_blocks = ((uint64_t)root->total_blocks + bits_per - 1U) / bits_per;
    if ((uint64_t)root->bitmap_base + bitmap_blocks > root->total_blocks) {
        set_error(error, error_size, "SFS bitmap extends beyond filesystem"); return -1;
    }
    analysis->bitmap_blocks = (uint32_t)bitmap_blocks;
    uint8_t *buf = malloc(root->block_size);
    if (buf == NULL) { set_error(error, error_size, "out of memory reading SFS bitmap"); return -1; }
    uint64_t free_blocks = 0U, used_blocks = 0U;
    uint64_t cell_index = 0U;
    for (uint64_t bi = 0U; bi < bitmap_blocks; ++bi) {
        const uint32_t block_no = root->bitmap_base + (uint32_t)bi;
        if (ld_pread_full(fd, buf, root->block_size, (uint64_t)block_no * root->block_size) != (ssize_t)root->block_size) {
            free(buf); set_error(error, error_size, "cannot read SFS bitmap block"); return -1;
        }
        if (infiltratr_load_be32(buf) != SFS_BITMAP_ID || infiltratr_load_be32(buf + 8U) != block_no ||
            !checksum_ok(buf, root->block_size, checksum_seed(root))) {
            free(buf); set_error(error, error_size, "invalid SFS bitmap block"); return -1;
        }
        const uint64_t first = bi * bits_per;
        uint64_t count = bits_per;
        if (first + count > root->total_blocks) count = root->total_blocks - first;
        for (uint64_t bit = 0U; bit < count; ++bit) {
            const uint64_t fs_block = first + bit;
            const uint8_t byte = buf[SFS_HEADER_BYTES + (size_t)(bit >> 3U)];
            const bool is_free = (byte & (uint8_t)(0x80U >> (bit & 7U))) != 0U;
            if (free_map != NULL) free_map[fs_block] = is_free ? 1U : 0U;
            if (is_free) free_blocks++; else used_blocks++;
            if (cells != NULL && cell_count != 0U) {
                while (cell_index + 1U < cell_count && fs_block > cells[cell_index].end) cell_index++;
                if (fs_block >= cells[cell_index].start && fs_block <= cells[cell_index].end) {
                    if (is_free) cells[cell_index].free_count++; else cells[cell_index].used_count++;
                }
            }
        }
    }
    free(buf);
    analysis->free_blocks = free_blocks; analysis->used_blocks = used_blocks;
    if (free_blocks + used_blocks != root->total_blocks) {
        set_error(error, error_size, "SFS bitmap accounting does not cover filesystem"); return -1;
    }
    (void)total_units;
    return 0;
}
int sfs_analyse(const char *path, SfsAnalysis *analysis, SfsMapCell *cells,
                uint64_t cell_count, char *error, size_t error_size) {
    if (path == NULL || analysis == NULL) { set_error(error, error_size, "invalid SFS analysis request"); return -1; }
    memset(analysis, 0, sizeof(*analysis));
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (error && error_size) (void)snprintf(error,error_size,"open: %s",strerror(errno)); return -1; }
    uint64_t physical = 0U;
    if (size_bytes(fd, path, &physical) != 0) { if (error&&error_size)(void)snprintf(error,error_size,"size: %s",strerror(errno)); close(fd); return -1; }
    Root root = {0}; bool pv=false,bv=false;
    if (discover_roots(fd, physical, &root, &pv, &bv, error, error_size) != 0) { close(fd); return -1; }
    const uint64_t fs_bytes = (uint64_t)root.total_blocks * root.block_size;
    const uint64_t physical_units = (physical + root.block_size - 1U) / root.block_size;
    const uint64_t total_units = physical_units > root.total_blocks ? physical_units : root.total_blocks;
    if (cells != NULL && cell_count != 0U) {
        for (uint64_t i=0U;i<cell_count;i++) {
            cells[i].start = i * total_units / cell_count;
            uint64_t endex = (i+1U) * total_units / cell_count;
            if (endex <= cells[i].start) endex = cells[i].start + 1U;
            cells[i].end = endex - 1U;
            cells[i].free_count=cells[i].used_count=cells[i].fragmented_count=0U;
            cells[i].outside_count=0U;
            const uint64_t outside_start = cells[i].start > root.total_blocks ? cells[i].start : root.total_blocks;
            if (endex > outside_start) cells[i].outside_count = endex - outside_start;
        }
    }
    analysis->block_size=root.block_size; analysis->total_blocks=root.total_blocks;
    analysis->bitmap_base=root.bitmap_base; analysis->root_object_container=root.root_object_container;
    analysis->admin_space_container=root.admin_space_container; analysis->extent_bnode_root=root.extent_bnode_root;
    analysis->object_node_root=root.object_node_root; analysis->structure_version=root.version;
    analysis->sequence_number=root.sequence; analysis->root_bits=root.bits; analysis->filesystem_bytes=fs_bytes;
    analysis->physical_bytes=physical; analysis->primary_root_valid=pv; analysis->backup_root_valid=bv;
    uint8_t *free_map = calloc(root.total_blocks, 1U);
    if (free_map == NULL) {
        close(fd);
        set_error(error, error_size, "out of memory tracking SFS allocation state");
        return -1;
    }
    if (validate_transaction(fd,&root,&analysis->transaction_pending,error,error_size)!=0 ||
        scan_bitmap(fd,&root,analysis,cells,cell_count,total_units,free_map,error,error_size)!=0 ||
        scan_catalogue(fd,&root,free_map,analysis,cells,cell_count,error,error_size)!=0) {
        free(free_map);
        close(fd);
        return -1;
    }
    free(free_map);
    close(fd); if(error&&error_size)error[0]='\0'; return 0;
}


static void model_close(SfsModel *model) {
    if (model->fd >= 0) (void)close(model->fd);
    free(model->free_map);
    free(model->extents.items);
    files_free(&model->files);
    memset(model, 0, sizeof(*model));
    model->fd = -1;
}

static int model_open(const char *path, SfsModel *model,
                      char *error, size_t error_size) {
    memset(model, 0, sizeof(*model));
    model->fd = -1;
    model->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (model->fd < 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "open: %s", strerror(errno));
        return -1;
    }
    if (size_bytes(model->fd, path, &model->physical_bytes) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "size: %s", strerror(errno));
        model_close(model);
        return -1;
    }
    bool primary_valid = false;
    bool backup_valid = false;
    if (discover_roots(model->fd, model->physical_bytes, &model->root,
                       &primary_valid, &backup_valid, error, error_size) != 0 ||
        validate_transaction(model->fd, &model->root,
                             &model->transaction_pending, error, error_size) != 0) {
        model_close(model);
        return -1;
    }

    model->free_map = calloc(model->root.total_blocks, 1U);
    if (model->free_map == NULL) {
        set_error(error, error_size, "out of memory tracking SFS allocation state");
        model_close(model);
        return -1;
    }
    SfsAnalysis analysis = {0};
    if (scan_bitmap(model->fd, &model->root, &analysis, NULL, 0U,
                    model->root.total_blocks, model->free_map,
                    error, error_size) != 0) {
        model_close(model);
        return -1;
    }

    uint8_t *tree_visited = calloc(model->root.total_blocks, 1U);
    if (tree_visited == NULL) {
        set_error(error, error_size, "out of memory tracking SFS extent B-tree");
        model_close(model);
        return -1;
    }
    uint32_t first_key = 0U;
    bool has_key = false;
    int rc = scan_extent_container(model->fd, &model->root,
                                   model->root.extent_bnode_root,
                                   tree_visited, 0U, &model->extents,
                                   &first_key, &has_key, error, error_size);
    free(tree_visited);
    if (rc != 0) {
        model_close(model);
        return -1;
    }
    for (size_t index = 0U; index < model->extents.count; ++index) {
        const SfsExtent *extent = &model->extents.items[index];
        if (index != 0U) {
            const SfsExtent *previous = &model->extents.items[index - 1U];
            if ((uint64_t)previous->key + previous->blocks > extent->key) {
                set_error(error, error_size, "SFS data extents overlap");
                model_close(model);
                return -1;
            }
        }
        for (uint32_t block = 0U; block < extent->blocks; ++block) {
            if (model->free_map[extent->key + block] != 0U) {
                set_error(error, error_size,
                          "SFS extent B-tree references bitmap-free data");
                model_close(model);
                return -1;
            }
        }
    }

    uint8_t *owned = calloc(model->extents.count == 0U ? 1U :
                            model->extents.count, 1U);
    if (owned == NULL) {
        set_error(error, error_size, "out of memory tracking SFS extent ownership");
        model_close(model);
        return -1;
    }
    analysis.growth_10_satisfied = true;
    rc = scan_object_catalogue(model->fd, &model->root, &model->extents,
                               owned, model->free_map, &analysis, NULL, 0U,
                               &model->files, error, error_size);
    if (rc == 0) {
        for (size_t index = 0U; index < model->extents.count; ++index) {
            if (owned[index] == 0U) {
                set_error(error, error_size,
                          "SFS extent B-tree contains unreferenced data");
                rc = -1;
                break;
            }
        }
    }
    free(owned);
    (void)first_key;
    (void)has_key;
    if (rc != 0) {
        model_close(model);
        return -1;
    }
    return 0;
}

static void fix_checksum(uint8_t *block, uint32_t block_size, uint32_t seed) {
    infiltratr_store_be32(block + 4U, 0U);
    uint32_t sum = seed;
    for (uint32_t offset = 0U; offset < block_size; offset += 4U)
        sum += infiltratr_load_be32(block + offset);
    infiltratr_store_be32(block + 4U, 0U - sum);
}

static int copy_bytes(int source_fd, int target_fd, uint64_t bytes,
                      char *error, size_t error_size) {
    uint8_t *buffer = malloc(SFS_IO_BATCH_BYTES);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory batching SFS image I/O");
        return -1;
    }
    int rc = 0;
    for (uint64_t offset = 0U; offset < bytes;) {
        uint64_t remaining = bytes - offset;
        size_t count = remaining > SFS_IO_BATCH_BYTES ?
                       SFS_IO_BATCH_BYTES : (size_t)remaining;
        if (ld_pread_full(source_fd, buffer, count, offset) != (ssize_t)count ||
            ld_pwrite_full(target_fd, buffer, count, offset) != (ssize_t)count) {
            if (error != NULL && error_size != 0U)
                (void)snprintf(error, error_size,
                               "short SFS image I/O at byte %" PRIu64, offset);
            rc = -1;
            break;
        }
        offset += count;
    }
    free(buffer);
    return rc;
}

static int choose_run(uint8_t *claimed, uint32_t total_blocks,
                      uint32_t need, uint32_t reserve, uint32_t *start) {
    const uint64_t span = (uint64_t)need + reserve;
    if (span == 0U) {
        *start = 0U;
        return 0;
    }
    for (uint32_t candidate = 0U;
         (uint64_t)candidate + span <= total_blocks; ++candidate) {
        bool available = true;
        for (uint32_t index = 0U; index < need + reserve; ++index) {
            if (claimed[candidate + index] != 0U) {
                candidate += index;
                available = false;
                break;
            }
        }
        if (!available) continue;
        *start = candidate;
        memset(claimed + candidate, 1, (size_t)need + reserve);
        return 0;
    }
    return -1;
}

static int copy_file_payload(const SfsModel *source, int stage_fd,
                             const SfsFile *file, uint32_t target_start,
                             char *error, size_t error_size) {
    const uint32_t batch_blocks =
        SFS_IO_BATCH_BYTES / source->root.block_size == 0U ? 1U :
        SFS_IO_BATCH_BYTES / source->root.block_size;
    uint8_t *buffer = malloc((size_t)batch_blocks * source->root.block_size);
    if (buffer == NULL) {
        set_error(error, error_size, "out of memory batching SFS file relocation");
        return -1;
    }
    uint32_t logical = 0U;
    for (size_t chain = 0U; chain < file->chain.count; ++chain) {
        const SfsExtent *extent =
            &source->extents.items[file->chain.items[chain]];
        uint32_t done = 0U;
        while (done < extent->blocks) {
            uint32_t count = extent->blocks - done;
            if (count > batch_blocks) count = batch_blocks;
            const size_t bytes = (size_t)count * source->root.block_size;
            const uint64_t source_offset =
                (uint64_t)(extent->key + done) * source->root.block_size;
            const uint64_t target_offset =
                (uint64_t)(target_start + logical) * source->root.block_size;
            if (ld_pread_full(source->fd, buffer, bytes, source_offset) !=
                    (ssize_t)bytes ||
                ld_pwrite_full(stage_fd, buffer, bytes, target_offset) !=
                    (ssize_t)bytes) {
                set_error(error, error_size, "short I/O relocating SFS file data");
                free(buffer);
                return -1;
            }
            done += count;
            logical += count;
        }
    }
    free(buffer);
    return 0;
}

static int extent_compare(const void *left, const void *right) {
    const SfsExtent *a = left;
    const SfsExtent *b = right;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return 0;
}

static int write_extent_records(int fd, const Root *root,
                                const SfsExtentVec *slots,
                                SfsExtent *records, size_t count,
                                char *error, size_t error_size) {
    if (count != slots->count) {
        set_error(error, error_size, "SFS relocation changed extent-record count");
        return -1;
    }
    if (count != 0U)
        qsort(records, count, sizeof(*records), extent_compare);
    uint8_t *block = malloc(root->block_size);
    if (block == NULL) {
        set_error(error, error_size, "out of memory rewriting SFS extent B-tree");
        return -1;
    }
    for (size_t index = 0U; index < count; ++index) {
        const SfsExtent *slot = &slots->items[index];
        const SfsExtent *record = &records[index];
        if (index != 0U) {
            const SfsExtent *previous = &records[index - 1U];
            if ((uint64_t)previous->key + previous->blocks > record->key) {
                free(block);
                set_error(error, error_size, "planned SFS extents overlap");
                return -1;
            }
        }
        if (read_metadata_block(fd, root, slot->container_block,
                                SFS_BNODE_ID, block, error, error_size) != 0) {
            free(block);
            return -1;
        }
        const uint8_t leaf_bytes = extent_node_bytes(root);
        if ((uint64_t)slot->node_offset + leaf_bytes >
            root->block_size || block[14U] == 0U ||
            block[15U] != leaf_bytes) {
            free(block);
            set_error(error, error_size, "SFS extent slot changed during staging");
            return -1;
        }
        uint8_t *node = block + slot->node_offset;
        infiltratr_store_be32(node, record->key);
        infiltratr_store_be32(node + 4U, record->next);
        infiltratr_store_be32(node + 8U, record->prev);
        if (root_is_sfs2(root))
            infiltratr_store_be32(node + 12U, record->blocks);
        else {
            if (record->blocks > UINT16_MAX) {
                free(block);
                set_error(error, error_size,
                          "SFS0 extent exceeds its 16-bit block-count field");
                return -1;
            }
            infiltratr_store_be16(node + 12U, (uint16_t)record->blocks);
        }
        fix_checksum(block, root->block_size, checksum_seed(root));
        if (ld_pwrite_full(fd, block, root->block_size,
                           (uint64_t)slot->container_block * root->block_size) !=
            (ssize_t)root->block_size) {
            free(block);
            set_error(error, error_size, "cannot rewrite SFS extent B-tree leaf");
            return -1;
        }
    }
    free(block);
    return 0;
}

static int refresh_btree_node(int fd, const Root *root, uint32_t block_no,
                              uint8_t *visited, unsigned depth,
                              uint32_t *first_key, bool *has_key,
                              char *error, size_t error_size) {
    if (depth > 64U || block_no >= root->total_blocks ||
        visited[block_no] != 0U) {
        set_error(error, error_size, "SFS extent B-tree topology changed during staging");
        return -1;
    }
    visited[block_no] = 1U;
    uint8_t *block = malloc(root->block_size);
    if (block == NULL) {
        set_error(error, error_size, "out of memory refreshing SFS extent B-tree");
        return -1;
    }
    if (read_metadata_block(fd, root, block_no, SFS_BNODE_ID, block,
                            error, error_size) != 0) {
        free(block);
        return -1;
    }
    const uint16_t count = infiltratr_load_be16(block + 12U);
    const bool leaf = block[14U] != 0U;
    const uint8_t node_size = block[15U];
    if ((leaf && node_size != extent_node_bytes(root)) ||
        (!leaf && node_size != SFS_INTERNAL_NODE_BYTES) ||
        (uint64_t)count * node_size >
            root->block_size - SFS_BNODE_HEADER_BYTES) {
        free(block);
        set_error(error, error_size, "invalid SFS extent B-tree geometry during refresh");
        return -1;
    }
    *has_key = false;
    if (leaf) {
        if (count != 0U) {
            *first_key = infiltratr_load_be32(block + SFS_BNODE_HEADER_BYTES);
            *has_key = true;
        }
        free(block);
        return 0;
    }

    for (uint16_t index = 0U; index < count; ++index) {
        uint8_t *node = block + SFS_BNODE_HEADER_BYTES +
                        (size_t)index * node_size;
        const uint32_t child = infiltratr_load_be32(node + 4U);
        uint32_t child_first = 0U;
        bool child_has_key = false;
        if (child == 0U ||
            refresh_btree_node(fd, root, child, visited, depth + 1U,
                               &child_first, &child_has_key,
                               error, error_size) != 0 ||
            !child_has_key) {
            free(block);
            if (error != NULL && error_size != 0U && error[0] == '\0')
                set_error(error, error_size, "invalid SFS B-tree child during refresh");
            return -1;
        }
        infiltratr_store_be32(node, index == 0U ? 0U : child_first);
        if (!*has_key) {
            *first_key = child_first;
            *has_key = true;
        }
    }
    fix_checksum(block, root->block_size, checksum_seed(root));
    if (ld_pwrite_full(fd, block, root->block_size,
                       (uint64_t)block_no * root->block_size) !=
        (ssize_t)root->block_size) {
        free(block);
        set_error(error, error_size, "cannot refresh SFS extent B-tree separators");
        return -1;
    }
    free(block);
    return 0;
}

static int refresh_btree(int fd, const Root *root,
                         char *error, size_t error_size) {
    uint8_t *visited = calloc(root->total_blocks, 1U);
    if (visited == NULL) {
        set_error(error, error_size, "out of memory refreshing SFS extent B-tree");
        return -1;
    }
    uint32_t first = 0U;
    bool has = false;
    const int rc = refresh_btree_node(fd, root, root->extent_bnode_root,
                                      visited, 0U, &first, &has,
                                      error, error_size);
    free(visited);
    (void)first;
    (void)has;
    return rc;
}

static int write_file_pointer(int fd, const Root *root, const SfsFile *file,
                              char *error, size_t error_size) {
    uint8_t *block = malloc(root->block_size);
    if (block == NULL) {
        set_error(error, error_size, "out of memory rewriting SFS object");
        return -1;
    }
    if (read_metadata_block(fd, root, file->object_block,
                            SFS_OBJECT_ID, block, error, error_size) != 0) {
        free(block);
        return -1;
    }
    if ((uint64_t)file->object_offset + object_fixed_bytes(root) >
            root->block_size ||
        infiltratr_load_be32(block + file->object_offset + 4U) != file->object_node) {
        free(block);
        set_error(error, error_size, "SFS file object changed during staging");
        return -1;
    }
    infiltratr_store_be32(block + file->object_offset + 12U,
                          file->chain.count == 0U ? 0U : file->new_start);
    fix_checksum(block, root->block_size, checksum_seed(root));
    if (ld_pwrite_full(fd, block, root->block_size,
                       (uint64_t)file->object_block * root->block_size) !=
        (ssize_t)root->block_size) {
        free(block);
        set_error(error, error_size, "cannot rewrite SFS file object");
        return -1;
    }
    free(block);
    return 0;
}

static int write_bitmap(int fd, const Root *root, const uint8_t *free_map,
                        char *error, size_t error_size) {
    const uint64_t bits_per =
        (uint64_t)(root->block_size - SFS_HEADER_BYTES) * 8U;
    const uint64_t bitmap_blocks =
        ((uint64_t)root->total_blocks + bits_per - 1U) / bits_per;
    uint8_t *block = malloc(root->block_size);
    if (block == NULL) {
        set_error(error, error_size, "out of memory rewriting SFS bitmap");
        return -1;
    }
    for (uint64_t bitmap_index = 0U; bitmap_index < bitmap_blocks;
         ++bitmap_index) {
        const uint32_t block_no = root->bitmap_base + (uint32_t)bitmap_index;
        if (read_metadata_block(fd, root, block_no, SFS_BITMAP_ID, block,
                                error, error_size) != 0) {
            free(block);
            return -1;
        }
        const uint64_t first = bitmap_index * bits_per;
        uint64_t count = bits_per;
        if (first + count > root->total_blocks)
            count = root->total_blocks - first;
        for (uint64_t bit = 0U; bit < count; ++bit) {
            const uint8_t mask = (uint8_t)(0x80U >> (bit & 7U));
            uint8_t *byte = block + SFS_HEADER_BYTES + (size_t)(bit >> 3U);
            if (free_map[first + bit] != 0U) *byte |= mask;
            else *byte &= (uint8_t)~mask;
        }
        fix_checksum(block, root->block_size, checksum_seed(root));
        if (ld_pwrite_full(fd, block, root->block_size,
                           (uint64_t)block_no * root->block_size) !=
            (ssize_t)root->block_size) {
            free(block);
            set_error(error, error_size, "cannot rewrite SFS allocation bitmap");
            return -1;
        }
    }
    free(block);
    return 0;
}

int sfs_build_stage(const char *source, const char *stage, bool growth,
                    unsigned growth_percent, bool live_updates,
                    uint64_t *commit_bytes, char *error, size_t error_size) {
    if (source == NULL || stage == NULL || growth_percent > 100U) {
        set_error(error, error_size, "invalid SFS staging request");
        return -1;
    }
    SfsModel model;
    if (model_open(source, &model, error, error_size) != 0)
        return -1;
    if (model.transaction_pending) {
        set_error(error, error_size,
                  "SFS filesystem has an unfinished native transaction marker");
        model_close(&model);
        return -1;
    }

    const uint64_t filesystem_bytes =
        (uint64_t)model.root.total_blocks * model.root.block_size;
    (void)unlink(stage);
    int stage_flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    stage_flags |= O_NOFOLLOW;
#endif
    int stage_fd = open(stage, stage_flags, 0600);
    if (stage_fd < 0 || ftruncate(stage_fd, (off_t)filesystem_bytes) != 0) {
        if (stage_fd >= 0) (void)close(stage_fd);
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "cannot create SFS working image: %s", strerror(errno));
        model_close(&model);
        return -1;
    }
    if (copy_bytes(model.fd, stage_fd, filesystem_bytes,
                   error, error_size) != 0) {
        (void)close(stage_fd);
        model_close(&model);
        return -1;
    }

    uint8_t *claimed = calloc(model.root.total_blocks, 1U);
    uint8_t *stage_free = malloc(model.root.total_blocks);
    SfsExtent *records = calloc(model.extents.count == 0U ? 1U :
                                model.extents.count, sizeof(*records));
    if (claimed == NULL || stage_free == NULL || records == NULL) {
        free(claimed);
        free(stage_free);
        free(records);
        (void)close(stage_fd);
        model_close(&model);
        set_error(error, error_size, "out of memory planning SFS relocation");
        return -1;
    }
    memcpy(stage_free, model.free_map, model.root.total_blocks);
    for (uint32_t block = 0U; block < model.root.total_blocks; ++block)
        claimed[block] = model.free_map[block] == 0U ? 1U : 0U;
    for (size_t index = 0U; index < model.extents.count; ++index) {
        const SfsExtent *extent = &model.extents.items[index];
        memset(claimed + extent->key, 0, extent->blocks);
        memset(stage_free + extent->key, 1, extent->blocks);
    }

    size_t record_count = 0U;
    int rc = 0;
    for (size_t file_index = 0U; file_index < model.files.count; ++file_index) {
        SfsFile *file = &model.files.items[file_index];
        const uint64_t need64 =
            ((uint64_t)file->byte_size + model.root.block_size - 1U) /
            model.root.block_size;
        if (need64 > UINT32_MAX) {
            set_error(error, error_size, "SFS file is too large for supported relocation");
            rc = -1;
            break;
        }
        const uint32_t need = (uint32_t)need64;
        const uint64_t reserve64 = growth && need != 0U ?
            ((uint64_t)need * growth_percent + 99U) / 100U : 0U;
        if (reserve64 > UINT32_MAX) {
            set_error(error, error_size, "SFS growth reserve is too large");
            rc = -1;
            break;
        }
        const uint32_t reserve = (uint32_t)reserve64;
        uint32_t start = 0U;
        if (need != 0U &&
            choose_run(claimed, model.root.total_blocks, need, reserve,
                       &start) != 0) {
            if (error != NULL && error_size != 0U)
                (void)snprintf(error, error_size,
                    "SFS layout cannot place object %u contiguously with its required reserve",
                    file->object_node);
            rc = -1;
            break;
        }
        file->new_start = start;
        if (need != 0U &&
            copy_file_payload(&model, stage_fd, file, start,
                              error, error_size) != 0) {
            rc = -1;
            break;
        }
        if (need != 0U) memset(stage_free + start, 0, need);
        if (reserve != 0U) memset(stage_free + start + need, 1, reserve);

        uint32_t cursor = start;
        for (size_t chain = 0U; chain < file->chain.count; ++chain) {
            const SfsExtent *old =
                &model.extents.items[file->chain.items[chain]];
            if (record_count >= model.extents.count) {
                set_error(error, error_size, "SFS extent planning overflow");
                rc = -1;
                break;
            }
            const uint32_t next = chain + 1U < file->chain.count ?
                                  cursor + old->blocks : 0U;
            const uint32_t prev = chain == 0U ? 0U :
                cursor - model.extents.items[file->chain.items[chain - 1U]].blocks;
            records[record_count++] = (SfsExtent){
                .key = cursor,
                .next = next,
                .prev = prev,
                .blocks = old->blocks,
                .container_block = 0U,
                .node_offset = 0U,
            };
            cursor += old->blocks;
        }
        if (rc != 0) break;
        if (write_file_pointer(stage_fd, &model.root, file,
                               error, error_size) != 0) {
            rc = -1;
            break;
        }
        if (live_updates && need != 0U) {
            (void)printf(
                "@@LIVE_RANGES {\"ranges\":[[%u,%u,1]],\"sequence\":%zu}\n",
                start, start + need, file_index + 1U);
            (void)fflush(stdout);
        }
    }
    if (rc == 0 && record_count != model.extents.count) {
        set_error(error, error_size,
                  "SFS object catalogue does not account for every extent record");
        rc = -1;
    }
    if (rc == 0 &&
        (write_extent_records(stage_fd, &model.root, &model.extents,
                              records, record_count, error, error_size) != 0 ||
         refresh_btree(stage_fd, &model.root, error, error_size) != 0 ||
         write_bitmap(stage_fd, &model.root, stage_free,
                      error, error_size) != 0 ||
         fsync(stage_fd) != 0)) {
        if (rc == 0 && error != NULL && error_size != 0U && error[0] == '\0')
            (void)snprintf(error, error_size,
                           "cannot sync SFS working image: %s", strerror(errno));
        rc = -1;
    }
    free(claimed);
    free(stage_free);
    free(records);
    (void)close(stage_fd);
    if (rc == 0 && commit_bytes != NULL) *commit_bytes = filesystem_bytes;
    model_close(&model);
    return rc;
}

int sfs_verify_layout(const char *path, bool growth, unsigned growth_percent,
                      char *error, size_t error_size) {
    if (growth && growth_percent != 10U) {
        set_error(error, error_size, "SFS Growth Defrag requires exactly 10 percent reserve");
        return -1;
    }
    SfsAnalysis analysis;
    if (sfs_analyse(path, &analysis, NULL, 0U, error, error_size) != 0)
        return -1;
    if (analysis.transaction_pending) {
        set_error(error, error_size, "SFS layout retains an unfinished native transaction marker");
        return -1;
    }
    if (analysis.fragmented_files != 0U) {
        set_error(error, error_size, "SFS layout remains fragmented");
        return -1;
    }
    if (growth && !analysis.growth_10_satisfied) {
        set_error(error, error_size,
                  "SFS layout does not provide the exact 10 percent post-file reserve");
        return -1;
    }
    return 0;
}

int sfs_commit_stage(const char *stage, const char *target, uint64_t *written,
                     char *error, size_t error_size) {
    SfsAnalysis analysis;
    if (sfs_analyse(stage, &analysis, NULL, 0U, error, error_size) != 0)
        return -1;
    int source_fd = open(stage, O_RDONLY | O_CLOEXEC);
    int target_fd = ld_device_open_verified_fd(target, true, NULL, 0U);
    if (source_fd < 0 || target_fd < 0) {
        if (source_fd >= 0) (void)close(source_fd);
        if (target_fd >= 0) (void)close(target_fd);
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "cannot open SFS stage or target: %s",
                           strerror(errno));
        return -1;
    }
    const int rc = copy_bytes(source_fd, target_fd, analysis.filesystem_bytes,
                              error, error_size);
    if (rc == 0 && fsync(target_fd) != 0) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size, "cannot sync SFS target: %s",
                           strerror(errno));
        (void)close(source_fd);
        (void)close(target_fd);
        return -1;
    }
    (void)close(source_fd);
    (void)close(target_fd);
    if (rc == 0 && written != NULL) *written = analysis.filesystem_bytes;
    return rc;
}

bool sfs_probe(const char *path)
{
    if (path == NULL) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint64_t physical = 0U;
    Root root = {0};
    bool primary_valid = false, backup_valid = false;
    const bool ok = size_bytes(fd, path, &physical) == 0 &&
        discover_roots(fd, physical, &root, &primary_valid, &backup_valid, NULL, 0U) == 0;
    (void)close(fd);
    return ok;
}
