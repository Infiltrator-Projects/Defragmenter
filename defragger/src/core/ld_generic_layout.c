/* SPDX-License-Identifier: GPL-3.0-or-later
 * Generic layout mathematics belongs to Defragmenter, not to any one SO.
 * The SO remains the sole interpreter and transactional writer for its FS.
 */
#include "ld_generic_layout.h"

#include <stdint.h>

/* The remainder route avoids computing units * percent before dividing. */
static int reserve_units(uint64_t units, unsigned percent, uint64_t *reserve)
{
    const uint64_t whole = units / 100u;
    const uint64_t remainder = units % 100u;
    if (whole > UINT64_MAX / percent) return LD_LAYOUT_INVALID;
    const uint64_t first = whole * percent;
    const uint64_t last = (remainder * percent + 99u) / 100u;
    if (first > UINT64_MAX - last) return LD_LAYOUT_INVALID;
    *reserve = first + last;
    return LD_LAYOUT_OK;
}

/* Two complete read-only planning passes: the second writes output only after
 * the first has proved every input and range can be represented. */
static int layout_pass(const LdGenericLayoutObject *objects, size_t count,
                       uint64_t first_unit, uint64_t limit_unit,
                       unsigned growth_percent,
                       LdGenericLayoutPlacement *out)
{
    uint64_t cursor = first_unit;
    uint64_t previous_id = 0u;
    for (size_t index = 0; index < count; ++index) {
        const LdGenericLayoutObject *object = &objects[index];
        if (!object->object_id || object->object_id <= previous_id ||
            !object->allocated_units || !object->alignment_units)
            return LD_LAYOUT_INVALID;
        previous_id = object->object_id;
        if (object->flags != 0u) return LD_LAYOUT_UNSUPPORTED;

        /* Align within the supplied data range, avoiding addition overflow. */
        const uint64_t remainder = cursor % object->alignment_units;
        const uint64_t pad =
            remainder ? object->alignment_units - remainder : 0u;
        if (cursor > limit_unit || pad > limit_unit - cursor)
            return LD_LAYOUT_NO_SPACE;
        cursor += pad;

        if (object->allocated_units > limit_unit - cursor)
            return LD_LAYOUT_NO_SPACE;
        uint64_t reserve = 0u;
        if (growth_percent != 0u &&
            reserve_units(object->allocated_units, growth_percent,
                          &reserve) != LD_LAYOUT_OK)
            return LD_LAYOUT_INVALID;

        if (out) {
            out[index].object_id = object->object_id;
            out[index].first_unit = cursor;
            out[index].allocated_units = object->allocated_units;
            out[index].gap_after_units = reserve;
        }
        cursor += object->allocated_units;
        if (reserve > limit_unit - cursor) return LD_LAYOUT_NO_SPACE;
        cursor += reserve;
    }
    return LD_LAYOUT_OK;
}

int ld_generic_layout_plan(const LdGenericLayoutObject *objects, size_t count,
                           uint64_t first_unit, uint64_t limit_unit,
                           unsigned growth_percent,
                           LdGenericLayoutPlacement *placements)
{
    if ((count != 0u && (!objects || !placements)) ||
        first_unit > limit_unit ||
        (growth_percent != 0u && growth_percent != 10u))
        return LD_LAYOUT_INVALID;
    const int valid = layout_pass(objects, count, first_unit,
                                  limit_unit, growth_percent, NULL);
    if (valid != LD_LAYOUT_OK) return valid;
    return layout_pass(objects, count, first_unit,
                       limit_unit, growth_percent, placements);
}


/* Size and range validation is independent of provider-specific allocation.
 * Range end is exclusive and must be representable without uint64_t wrap. */
static int check_free_ranges(const LdGenericFreeExtent *ranges, size_t count)
{
    uint64_t previous_end = 0u;
    for (size_t i = 0u; i < count; ++i) {
        const uint64_t first = ranges[i].first_unit;
        const uint64_t length = ranges[i].allocated_units;
        if (length == 0u || first > UINT64_MAX - length ||
            (i != 0u && first < previous_end))
            return LD_LAYOUT_INVALID;
        previous_end = first + length;
    }
    return LD_LAYOUT_OK;
}

static void usable_range(const LdGenericFreeExtent *range,
                         uint64_t alignment, uint64_t *first,
                         uint64_t *capacity)
{
    const uint64_t modulo = range->first_unit % alignment;
    const uint64_t padding = modulo ? alignment - modulo : 0u;
    if (padding > range->allocated_units) {
        *first = range->first_unit;
        *capacity = 0u;
    } else {
        *first = range->first_unit + padding;
        *capacity = range->allocated_units - padding;
    }
}

/* A full single-extent fit always wins over a split fit. The result order is
 * deterministic, smallest physical starting block first. */
static int plan_file_extent_pass(const LdGenericLayoutObject *object,
                                 const LdGenericFreeExtent *free_extents,
                                 size_t free_count, uint64_t gap,
                                 LdGenericPlannedExtent *output,
                                 size_t *extents)
{
    for (size_t i = 0u; i < free_count; ++i) {
        uint64_t first = 0u, capacity = 0u;
        usable_range(&free_extents[i], object->alignment_units,
                     &first, &capacity);
        if (object->allocated_units <= capacity &&
            gap <= capacity - object->allocated_units) {
            if (output) {
                output[0] = (LdGenericPlannedExtent){
                    first, object->allocated_units, gap
                };
            }
            *extents = 1u;
            return LD_LAYOUT_OK;
        }
    }

    /* Fallback is a split file, never a split Growth reserve. Every data
     * extent is individually aligned. The final extent must have its
     * adjacent reserve in the SAME certified free range. */
    uint64_t remaining = object->allocated_units;
    size_t count = 0u;
    for (size_t i = 0u; i < free_count && remaining; ++i) {
        uint64_t first = 0u, capacity = 0u;
        usable_range(&free_extents[i], object->alignment_units,
                     &first, &capacity);
        if (capacity == 0u) continue;
        const int finish = remaining <= capacity &&
                           gap <= capacity - remaining;
        uint64_t take = capacity < remaining ? capacity : remaining;
        if (!finish && take == remaining) {
            /* Keep at least one data unit for an extent that also fits
             * the final free reserve. This avoids a fake Growth success. */
            if (take == 1u) continue;
            --take;
        }
        if (take == 0u) continue;
        if (count == SIZE_MAX) return LD_LAYOUT_INVALID;
        if (output) {
            output[count] = (LdGenericPlannedExtent){
                first, take, finish ? gap : 0u
            };
        }
        ++count;
        remaining -= take;
        if (finish) {
            *extents = count;
            return LD_LAYOUT_OK;
        }
    }
    return LD_LAYOUT_NO_SPACE;
}

int ld_generic_layout_plan_file_extents(
    const LdGenericLayoutObject *object,
    const LdGenericFreeExtent *free_extents, size_t free_count,
    unsigned growth_percent,
    LdGenericPlannedExtent *output, size_t output_capacity,
    size_t *written)
{
    if (!object || !written || (free_count && !free_extents) ||
        (growth_percent != 0u && growth_percent != 10u) ||
        (output_capacity && !output) ||
        !object->object_id || !object->allocated_units ||
        !object->alignment_units)
        return LD_LAYOUT_INVALID;
    if (object->flags) return LD_LAYOUT_UNSUPPORTED;
    if (check_free_ranges(free_extents, free_count) != LD_LAYOUT_OK)
        return LD_LAYOUT_INVALID;

    uint64_t gap = 0u;
    if (growth_percent != 0u &&
        reserve_units(object->allocated_units, growth_percent,
                      &gap) != LD_LAYOUT_OK)
        return LD_LAYOUT_INVALID;

    size_t required = 0u;
    const int first = plan_file_extent_pass(object, free_extents,
                                             free_count, gap, NULL, &required);
    if (first != LD_LAYOUT_OK) return first;
    if (required > output_capacity) return LD_LAYOUT_OUTPUT_TOO_SMALL;
    size_t actual = 0u;
    const int second = plan_file_extent_pass(object, free_extents,
                                              free_count, gap, output, &actual);
    if (second != LD_LAYOUT_OK || actual != required)
        return LD_LAYOUT_INVALID;
    *written = required;
    return LD_LAYOUT_OK;
}
