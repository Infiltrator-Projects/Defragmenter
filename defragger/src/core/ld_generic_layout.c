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
