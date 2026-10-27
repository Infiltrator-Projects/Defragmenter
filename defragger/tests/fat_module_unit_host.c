// SPDX-License-Identifier: GPL-3.0-or-later
/* Supplies the same generic host services to filesystem-source unit tests. */
#include "fat_defrag_abi.h"

const IfsFatDefragHostV1 *ifs_fat_defrag_host;

__attribute__((constructor)) static void prepare_fat_test_host(void) {
    static IfsFatDefragHostV1 host = {
        .abi_version = IFS_FAT_DEFRAG_ABI_VERSION,
        .structure_bytes = sizeof(IfsFatDefragHostV1),
        .program_version = "unit-test",
#define IFS_FAT_BIND_HOST(name) .name = name,
        IFS_FAT_DEFRAG_HOST_FUNCTIONS(IFS_FAT_BIND_HOST)
#undef IFS_FAT_BIND_HOST
    };
    host.out = stdout;
    host.err = stderr;
    ifs_fat_defrag_host = &host;
}
