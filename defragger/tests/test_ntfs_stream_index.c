// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static void check_first_fit_equivalence(bool growth) {
    NtfsStream streams[32] = {0};
    uint8_t original[128], expected[128];
    memset(original, 0, sizeof(original));
    NtfsLayout layout = {0}; layout.bitmap_bytes = sizeof(original);
    layout.bitmap = original;
    NtfsLayout reference = layout; reference.bitmap = expected;
    for (uint64_t cluster = 0U; cluster < 1024U; ++cluster)
        if (cluster % 19U < 5U) ntfs_bitmap_set(&layout, cluster, true);
    memcpy(expected, original, sizeof(original));
    for (size_t index = 0U; index < 32U; ++index) {
        NtfsStream *stream = &streams[31U - index];
        stream->record_number = 30U + index;
        stream->attribute_type = NTFS_ATTR_DATA;
        stream->movable = true; stream->clusters = index % 7U + 1U;
    }
    NtfsCatalogue catalogue = {0}; catalogue.items = streams; catalogue.count = 32U;
    NtfsPlacementVec placements = {0}; char *error = NULL;
    CHECK(ntfs_plan_layout(&layout, &catalogue, 1024U, growth, &placements, &error) == 0);
    for (size_t item = 0U; item < 32U; ++item) {
        const uint64_t clusters = item % 7U + 1U;
        const uint64_t span = clusters + (growth ? 1U : 0U);
        uint64_t start = 1U;
        for (; start + span <= 1023U; ++start) {
            bool legal = true;
            for (uint64_t part = 0U; part < span; ++part)
                if (ntfs_bitmap_bit(&reference, start + part)) legal = false;
            if (legal) break;
        }
        CHECK(start + span <= 1023U);
        CHECK(placements.items[item].record_number == item + 30U);
        CHECK(placements.items[item].start == start);
        CHECK(placements.items[item].reserve == (growth ? 1U : 0U));
        for (uint64_t part = 0U; part < span; ++part)
            ntfs_bitmap_set(&reference, start + part, true);
        for (uint64_t part = 0U; part < clusters; ++part)
            CHECK(ntfs_bitmap_bit(&layout, start + part));
        if (growth) CHECK(!ntfs_bitmap_bit(&layout, start + clusters));
    }
    ntfs_placements_free(&placements);
}

int main(void) {
    check_first_fit_equivalence(false);
    check_first_fit_equivalence(true);
    NtfsStream streams[7] = {0};
    for (size_t index = 0U; index < 7U; ++index) {
        streams[index].record_number = 30U;
        streams[index].attribute_type = NTFS_ATTR_DATA;
    }
    streams[1].record_number = 40U;
    streams[1].base_record = 30U; /* Extension of the unnamed stream. */
    streams[1].lowest_vcn = 12U;
    strcpy(streams[2].attribute_name, "named");
    strcpy(streams[3].attribute_name, "named");
    streams[3].base_record = 30U;
    streams[3].record_number = 41U;
    strcpy(streams[4].attribute_name, "Named"); /* Names remain case-sensitive. */
    streams[5].attribute_type = NTFS_ATTR_INDEX_ALLOCATION;
    streams[6].record_number = 31U;
    NtfsCatalogue catalogue = {0};
    catalogue.items = streams; catalogue.count = 7U;
    const size_t expected[] = {2U, 2U, 2U, 2U, 1U, 1U, 1U};
    size_t *counts = ntfs_logical_stream_counts(&catalogue);
    for (size_t index = 0U; index < 7U; ++index) CHECK(counts[index] == expected[index]);
    free(counts);

    /* An ordinary desktop catalogue must not trigger a search per stream. */
    catalogue.count = 100000U;
    catalogue.items = calloc(catalogue.count, sizeof(*catalogue.items));
    CHECK(catalogue.items != NULL);
    for (size_t index = 0U; index < catalogue.count; ++index) {
        catalogue.items[index].record_number = catalogue.count - index;
        catalogue.items[index].attribute_type = NTFS_ATTR_DATA;
    }
    const clock_t started = clock();
    counts = ntfs_logical_stream_counts(&catalogue);
    for (size_t index = 0U; index < catalogue.count; ++index) CHECK(counts[index] == 1U);
    printf("Indexed 100000 NTFS streams in %.3f CPU seconds\n", (double)(clock() - started) / CLOCKS_PER_SEC);
    free(counts);
    NtfsLayout layout = {0};
    const uint64_t total = catalogue.count * 2U + 4U;
    layout.bitmap_bytes = (size_t)((total + 7U) / 8U);
    layout.bitmap = malloc(layout.bitmap_bytes);
    CHECK(layout.bitmap != NULL);
    memset(layout.bitmap, 0x55, layout.bitmap_bytes);
    for (size_t index = 0U; index < catalogue.count; ++index) {
        catalogue.items[index].movable = true;
        catalogue.items[index].clusters = 1U;
    }
    NtfsPlacementVec placements = {0};
    char *error = NULL;
    const clock_t plan_started = clock();
    CHECK(ntfs_plan_layout(&layout, &catalogue, total, false, &placements, &error) == 0);
    CHECK(placements.count == catalogue.count);
    for (size_t index = 0U; index < placements.count; ++index) {
        CHECK(placements.items[index].record_number == index + 1U);
        CHECK(placements.items[index].start == index * 2U + 1U);
        CHECK(placements.items[index].clusters == 1U);
    }
    printf("Placed 100000 canonical streams across 100000 isolated free runs in %.3f CPU seconds\n",
           (double)(clock() - plan_started) / CLOCKS_PER_SEC);
    ntfs_placements_free(&placements);
    free(layout.bitmap); free(catalogue.items);
    return 0;
}
