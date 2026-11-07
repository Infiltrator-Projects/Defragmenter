/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_DEFRAG_MODULE_H
#define IFS_DEFRAG_MODULE_H
#include <stddef.h>
#include <stdint.h>

/* Process-hosted filesystem operations. This interface supplies mechanisms,
 * never a filesystem parser, placement policy or recovery interpretation.
 * The same ELF object also exports its independent firmware Boot interface.
 * One operation runs in a child process; keep the object loaded until exit. */
#define IFS_DEFRAG_ABI_V2 2u
#define IFS_DEFRAG_STOPPED 130
#define IFS_DEFRAG_PATH_BYTES 4096u
#define IFS_DEFRAG_ID_BYTES 512u
typedef struct {
    void *handle;
    uint64_t bytes;
    char path[IFS_DEFRAG_PATH_BYTES];
    char identity[IFS_DEFRAG_ID_BYTES];
} IfsDefragTarget;

/* Generic layout exchange types. The SO provides an exact, certified object
 * inventory and allocation-unit boundaries. Defragmenter owns policy; the SO
 * retains ALL authority for allocation validity, metadata and durable moves.
 * A successful plan is informational only and cannot authorize disk writes.
 * These fixed structs form the optional layout-planning host extension. */
#define IFS_DEFRAG_LAYOUT_OBJECT_PINNED 1u
#define IFS_DEFRAG_LAYOUT_OBJECT_SHARED 2u
#define IFS_DEFRAG_LAYOUT_OBJECT_UNSUPPORTED 4u
typedef struct {
    uint64_t object_id;
    uint64_t allocated_units;
    uint64_t alignment_units;
    uint32_t flags;
} IfsDefragLayoutObject;
typedef struct {
    uint64_t object_id;
    uint64_t first_unit;
    uint64_t allocated_units;
    uint64_t gap_after_units;
} IfsDefragLayoutPlacement;

/* A provider-certified, sorted, disjoint free interval in filesystem
 * allocation units. System metadata, reserved regions, snapshots and other
 * unavailable storage are absent from this inventory. */
typedef struct {
    uint64_t first_unit;
    uint64_t allocated_units;
} IfsDefragFreeExtent;

/* Ordered data extents for one file. Only the final extent may reserve free
 * growth units immediately after its last data unit. A reserved gap is NOT
 * allocated or copied into the output filesystem. */
typedef struct {
    uint64_t first_unit;
    uint64_t allocated_units;
    uint64_t gap_after_units;
} IfsDefragPlannedExtent;

typedef struct {
    uint32_t abi_version, structure_bytes;
    void *context;
    const char *program_version;
    void *(*allocate)(size_t);
    void (*release)(void *);
    int (*open_target)(void *, const char *, int, const char *, uint64_t, IfsDefragTarget *);
    int (*create_stage)(void *, const char *, uint64_t, IfsDefragTarget *);
    void (*close_target)(void *, IfsDefragTarget *);
    int (*read_at)(void *, const IfsDefragTarget *, uint64_t, void *, uint32_t);
    int (*write_at)(void *, const IfsDefragTarget *, uint64_t, const void *, uint32_t);
    int (*sync_target)(void *, const IfsDefragTarget *);
    int (*file_exists)(void *, const char *); /* 1 exists, 0 absent, -1 error. */
    int (*read_file)(void *, const char *, void *, uint32_t, uint32_t *);
    int (*atomic_file)(void *, const char *, const void *, uint32_t);
    int (*remove_file)(void *, const char *);
    void *(*digest_create)(void *);
    int (*digest_update)(void *, void *, const void *, size_t);
    int (*digest_finish)(void *, void *, uint8_t[32]);
    void (*digest_release)(void *, void *);
    int (*stop_requested)(void *);
    int (*print)(void *, int, const char *, ...); /* 0 stdout, 1 stderr. */
    int (*format)(char *, size_t, const char *, ...);
    /* Optional appended extension; check structure_bytes before access.
     * Return: 0 success, -1 malformed/overflow, -2 no space,
     * -3 unsupported/pinned/shared. Output unchanged on failure.
     * growth_percent is 0 for Defrag or 10 for Growth Defrag.
     * No filesystem mutations, no allocation authority, no side effects.
     */
    int (*plan_layout)(const IfsDefragLayoutObject *, size_t,
                       uint64_t first_unit, uint64_t limit_unit,
                       unsigned growth_percent, IfsDefragLayoutPlacement *);
    /* Optional, appended range-aware host service. The SO supplies certified
     * ordered/disjoint free ranges; Defragmenter chooses either one contiguous
     * run or data extents split around reserved/pinned regions. No mutation.
     * Returns 0 on a completely valid plan, -1 invalid/overflow, -2 no space,
     * -3 unsupported objects, -4 insufficient output slots. On failure neither
     * the planned extents nor *written is modified.
     * Existing older V2 hosts omit this field: probe structure_bytes.
     */
    int (*plan_file_extents)(const IfsDefragLayoutObject *,
                             const IfsDefragFreeExtent *, size_t free_count,
                             unsigned growth_percent,
                             IfsDefragPlannedExtent *, size_t output_capacity,
                             size_t *written);
} IfsDefragHostV2;
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id;
    uint64_t capabilities;
    int (*run)(const IfsDefragHostV2 *, int, char **);
} IfsDefragModuleV2;
#define IFS_DEFRAG_ANALYSE UINT64_C(1)
#define IFS_DEFRAG_RELAYOUT UINT64_C(2)
#define IFS_DEFRAG_GROWTH UINT64_C(4)
#define IFS_DEFRAG_RECOVER UINT64_C(8)
const IfsDefragModuleV2 *infiltrator_filesystem_defrag_v2(void);
#endif
