/* SPDX-License-Identifier: GPL-3.0-or-later
 * Pure-memory regression: no filesystem device or privileged operations.
 */
#include "ld_generic_layout.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "generic layout check failed: %s at %s:%d\\n", \
                #expression, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

static void basic_defrag_growth(void)
{
    const LdGenericLayoutObject objects[] = {
        {1u, 100u, 1u, 0u}, {2u, 19u, 1u, 0u}, {3u, 1u, 1u, 0u}
    };
    LdGenericLayoutPlacement output[3] = {0};
    CHECK(ld_generic_layout_plan(objects, 3u, 7u, 500u, 0u, output)
           == LD_LAYOUT_OK);
    CHECK(output[0].first_unit == 7u && output[1].first_unit == 107u);
    CHECK(output[2].first_unit == 126u && !output[2].gap_after_units);

    CHECK(ld_generic_layout_plan(objects, 3u, 7u, 500u, 10u, output)
           == LD_LAYOUT_OK);
    CHECK(output[0].first_unit == 7u && output[0].gap_after_units == 10u);
    CHECK(output[1].first_unit == 117u && output[1].gap_after_units == 2u);
    CHECK(output[2].first_unit == 138u && output[2].gap_after_units == 1u);
    CHECK(output[0].object_id == 1u && output[2].object_id == 3u);
}

static void alignment_and_exclusive_limit(void)
{
    const LdGenericLayoutObject objects[] = {
        {1u, 100u, 1u, 0u}, {2u, 19u, 8u, 0u}
    };
    LdGenericLayoutPlacement output[2] = {0};
    CHECK(ld_generic_layout_plan(objects, 2u, 7u, 141u, 10u, output)
           == LD_LAYOUT_OK);
    CHECK(output[1].first_unit == 120u && output[1].gap_after_units == 2u);
    CHECK(ld_generic_layout_plan(objects, 2u, 7u, 140u, 10u, output)
           == LD_LAYOUT_NO_SPACE);
}

static void failures_never_publish_partial_plan(void)
{
    const LdGenericLayoutPlacement original = {
        UINT64_C(0x11111111), UINT64_C(0x22222222),
        UINT64_C(0x33333333), UINT64_C(0x44444444)
    };
    LdGenericLayoutPlacement output[2] = {original, original};
    LdGenericLayoutObject objects[] = {
        {1u, 4u, 1u, 0u}, {2u, 200u, 1u, 0u}
    };
    CHECK(ld_generic_layout_plan(objects, 2u, 0u, 100u, 0u, output)
           == LD_LAYOUT_NO_SPACE);
    CHECK(memcmp(&output[0], &original, sizeof(original)) == 0);
    CHECK(memcmp(&output[1], &original, sizeof(original)) == 0);

    objects[1].allocated_units = 4u;
    objects[1].object_id = 1u;
    CHECK(ld_generic_layout_plan(objects, 2u, 0u, 100u, 0u, output)
           == LD_LAYOUT_INVALID);
    CHECK(memcmp(&output[0], &original, sizeof(original)) == 0);
    objects[1].object_id = 2u;
    objects[1].flags = LD_LAYOUT_OBJECT_SHARED;
    CHECK(ld_generic_layout_plan(objects, 2u, 0u, 100u, 0u, output)
           == LD_LAYOUT_UNSUPPORTED);
    CHECK(memcmp(&output[0], &original, sizeof(original)) == 0);
    objects[1].flags = LD_LAYOUT_OBJECT_PINNED;
    CHECK(ld_generic_layout_plan(objects, 2u, 0u, 100u, 0u, output)
           == LD_LAYOUT_UNSUPPORTED);
}

static void invalid_and_overflow_inputs(void)
{
    LdGenericLayoutObject object = {1u, UINT64_MAX, 1u, 0u};
    LdGenericLayoutPlacement output = {0};
    CHECK(ld_generic_layout_plan(&object, 1u, 0u, UINT64_MAX, 10u, &output)
           == LD_LAYOUT_NO_SPACE);
    object.allocated_units = 1u;
    CHECK(ld_generic_layout_plan(&object, 1u, UINT64_MAX - 1u,
                                  UINT64_MAX, 10u, &output)
           == LD_LAYOUT_NO_SPACE);
    CHECK(ld_generic_layout_plan(&object, 1u, 0u, 4u, 11u, &output)
           == LD_LAYOUT_INVALID);
    object.alignment_units = 0u;
    CHECK(ld_generic_layout_plan(&object, 1u, 0u, 4u, 0u, &output)
           == LD_LAYOUT_INVALID);
    CHECK(ld_generic_layout_plan(&object, 1u, 0u, 4u, 0u, NULL)
           == LD_LAYOUT_INVALID);
    CHECK(ld_generic_layout_plan(NULL, 0u, 0u, 0u, 0u, NULL)
           == LD_LAYOUT_OK);
}

int main(void)
{
    basic_defrag_growth();
    alignment_and_exclusive_limit();
    failures_never_publish_partial_plan();
    invalid_and_overflow_inputs();
    puts("generic layout planner: packing, 10% gaps, alignment and fail-closed checks passed");
    return 0;
}
