// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

int main(void) {
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
    free(counts); free(catalogue.items);
    return 0;
}
