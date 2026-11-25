// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"
#include <string.h>

/* Read-only policy query; Test Media does not clear filesystem signatures
 * through host utilities or attempt to initialise unsupported partitions. */
int ldtm_is_reserved_partition_label(const char *label)
{
    if (!label) return 0;
    for (size_t i = 0; i < ldtm_spec_count(); ++i) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[i];
        if (spec->creator == LDTM_CREATOR_MANUAL &&
            strcmp(spec->label, label) == 0)
            return 1;
    }
    return 0;
}
