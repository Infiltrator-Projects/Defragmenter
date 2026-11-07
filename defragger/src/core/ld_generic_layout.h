/* SPDX-License-Identifier: GPL-3.0-or-later
 * Filesystem-independent canonical placement policy.
 *
 * The filesystem SO supplies an ordered sequence of individually movable
 * file objects in its own allocation units. Defragmenter decides their
 * target placement. This module never reads a disk, updates metadata, or
 * assumes that copying physical sectors constitutes a valid transaction.
 */
#ifndef LD_GENERIC_LAYOUT_H
#define LD_GENERIC_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
/* Reuse the one public binary layout definition; avoid two subtly different
 * representations between the generic application and filesystem modules. */
#include "../../native/filesystem_defrag_abi.h"

enum {
    LD_LAYOUT_OK = 0,
    LD_LAYOUT_INVALID = -1,
    LD_LAYOUT_NO_SPACE = -2,
    LD_LAYOUT_UNSUPPORTED = -3
};

/* Input objects MUST be strictly ordered by nonzero stable object_id.
 * A provider must not silently remove pinned/shared objects: flags are
 * explicit and currently cause safe refusal until a constrained planner
 * can model them. Only an SO may certify that a file is independently
 * movable; a ZFS shared block, snapshot reference or metadata block is
 * not independently movable by an arbitrary byte copy. */
enum {
    LD_LAYOUT_OBJECT_PINNED = IFS_DEFRAG_LAYOUT_OBJECT_PINNED,
    LD_LAYOUT_OBJECT_SHARED = IFS_DEFRAG_LAYOUT_OBJECT_SHARED,
    LD_LAYOUT_OBJECT_UNSUPPORTED = IFS_DEFRAG_LAYOUT_OBJECT_UNSUPPORTED
};
typedef IfsDefragLayoutObject LdGenericLayoutObject;
typedef IfsDefragLayoutPlacement LdGenericLayoutPlacement;
typedef IfsDefragFreeExtent LdGenericFreeExtent;
typedef IfsDefragPlannedExtent LdGenericPlannedExtent;

/* [first_unit, limit_unit) is a provider-certified relocatable data region
 * in abstract allocation units. Each file is packed contiguously in input
 * order, with no reserve for Defrag, or a rounded-up 10% adjacent free
 * reserve after EVERY file for Growth Defrag. Provider-specified alignment
 * may cause additional gaps. The provider must validate real allocations,
 * reserved extents and topology BEFORE using the result.
 *
 * This is PLAN-ONLY: the output cannot be executed without an SO-supplied
 * durable relocation/commit/recovery protocol and independent verification.
 * Failure leaves every output element unchanged (two-pass preflight).
 */
int ld_generic_layout_plan(const LdGenericLayoutObject *objects, size_t count,
                           uint64_t first_unit, uint64_t limit_unit,
                           unsigned growth_percent,
                           LdGenericLayoutPlacement *placements);

/* Plan the data extents of a single file around multiple disjoint provider-
 * certified free ranges. Prefer the earliest complete contiguous extent. If
 * impossible, deterministically split across ascending free runs and reserve
 * the 10% Growth gap *after the final data extent*.
 *
 * Preflight validates every range and verifies space/capacity BEFORE writing
 * output. Neither extents nor *written are modified on failure.
 * Returns LD_LAYOUT_OK, INVALID, NO_SPACE, UNSUPPORTED or OUTPUT_TOO_SMALL.
 * This function is plan-only; no I/O or filesystem-specific decisions.
 */
#define LD_LAYOUT_OUTPUT_TOO_SMALL (-4)
int ld_generic_layout_plan_file_extents(
    const LdGenericLayoutObject *object,
    const LdGenericFreeExtent *free_extents, size_t free_count,
    unsigned growth_percent,
    LdGenericPlannedExtent *output, size_t output_capacity,
    size_t *written);

#endif
