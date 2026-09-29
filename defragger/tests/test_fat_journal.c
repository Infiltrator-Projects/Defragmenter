// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fat_journal.h"
#include "ld_runtime.h"


static int fail(const char *message) {
    fprintf(stderr, "fat journal test failed: %s\n", message);
    return 1;
}


int main(void) {
    char path[] = "/tmp/linux-defragger-fat-journal.XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor < 0) return fail("mkstemp");
    close(descriptor);
    unlink(path);

    RelocationJournal source = {
        .device_path = ld_xstrdup("/dev/test-fat"),
        .target_identity = ld_xstrdup("regular:8:1:4242"),
        .physical_bytes = UINT64_C(2147483648),
        .volume_id = UINT32_C(0x1234ABCD),
        .stage = J_DEST_LINKED,
        .root_old = 5,
        .root_new = 7,
    };
    relocation_journal_add_move(
        &source,
        (RelocationMove){
            .source = 5,
            .destination = 7,
            .next = 9,
            .predecessor = 3,
        }
    );
    relocation_journal_add_move(
        &source,
        (RelocationMove){
            .source = 9,
            .destination = 8,
            .next = UINT32_C(0x0FFFFFFF),
            .predecessor = 5,
        }
    );
    relocation_journal_add_dir_patch(
        &source,
        (RelocationDirPatch){
            .offset = 4096,
            .old_target = 5,
            .new_target = 7,
        }
    );

    relocation_journal_write(path, &source);
    if (!path_exists(path)) return fail("journal was not created");
    if (!journal_has_magic(path, RELOCATION_JOURNAL_MAGIC)) {
        return fail("journal magic was not preserved");
    }

    RelocationJournal loaded = relocation_journal_read(path);
    if (
        strcmp(loaded.device_path, source.device_path) != 0
        || strcmp(loaded.target_identity, source.target_identity) != 0
        || loaded.physical_bytes != source.physical_bytes
        || loaded.volume_id != source.volume_id
        || loaded.stage != source.stage
        || loaded.root_old != source.root_old
        || loaded.root_new != source.root_new
        || loaded.move_count != source.move_count
        || loaded.dir_patch_count != source.dir_patch_count
        || memcmp(
            loaded.moves,
            source.moves,
            source.move_count * sizeof(*source.moves)
        ) != 0
        || memcmp(
            loaded.dir_patches,
            source.dir_patches,
            source.dir_patch_count * sizeof(*source.dir_patches)
        ) != 0
    ) {
        return fail("round-trip changed a transaction field");
    }

    journal_remove(path);
    if (path_exists(path)) return fail("journal removal was not durable");
    relocation_journal_free(&loaded);
    relocation_journal_free(&source);
    puts("FAT journal round-trip test passed");
    return 0;
}
