// SPDX-License-Identifier: GPL-3.0-or-later
/* Exercise the real mount parsers with synthetic mountinfo/sysfs inputs.
 * No mount privilege, device node or production override is needed. */
#include <stdio.h>
#include <string.h>

static const char *fixture_mountinfo;
static FILE *fixture_fopen(const char *path, const char *mode) {
    return fopen(strcmp(path, "/proc/self/mountinfo") == 0
                     ? fixture_mountinfo : path, mode);
}
#define fopen fixture_fopen
#include "../src/core/ld_device.c"
#undef fopen

#define REQUIRE(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "mounted-image identity test failed: %s\n", #expression); \
        return 1; \
    } \
} while (0)

int main(void) {
    char directory[] = "/tmp/defragger-mount-identity.XXXXXX";
    REQUIRE(mkdtemp(directory) != NULL);
    char original[PATH_MAX], alias[PATH_MAX], other[PATH_MAX];
    char loop[PATH_MAX], backing[PATH_MAX], mountinfo[PATH_MAX];
    REQUIRE(snprintf(original, sizeof(original), "%s/original.img", directory) > 0);
    REQUIRE(snprintf(alias, sizeof(alias), "%s/alias.img", directory) > 0);
    REQUIRE(snprintf(other, sizeof(other), "%s/other.img", directory) > 0);
    REQUIRE(snprintf(loop, sizeof(loop), "%s/loop", directory) > 0);
    REQUIRE(snprintf(backing, sizeof(backing), "%s/loop/backing_file", directory) > 0);
    REQUIRE(snprintf(mountinfo, sizeof(mountinfo), "%s/mountinfo", directory) > 0);
    FILE *file = fopen(original, "w");
    REQUIRE(file != NULL && fclose(file) == 0);
    file = fopen(other, "w");
    REQUIRE(file != NULL && fclose(file) == 0);
    REQUIRE(link(original, alias) == 0);
    REQUIRE(mkdir(loop, 0700) == 0);
    file = fopen(backing, "w");
    REQUIRE(file != NULL);
    REQUIRE(fprintf(file, "%s\n", original) > 0 && fclose(file) == 0);
    file = fopen(mountinfo, "w");
    REQUIRE(file != NULL);
    REQUIRE(fprintf(file, "1 0 7:0 / /mnt rw - vfat %s rw\n", original) > 0);
    REQUIRE(fclose(file) == 0);
    fixture_mountinfo = mountinfo;

    struct stat original_status, alias_status, other_status;
    REQUIRE(stat(original, &original_status) == 0);
    REQUIRE(stat(alias, &alias_status) == 0);
    REQUIRE(stat(other, &other_status) == 0);
    REQUIRE(ld_mount_source_matches_regular_identity(&original_status));
    REQUIRE(ld_mount_source_matches_regular_identity(&alias_status));
    REQUIRE(!ld_mount_source_matches_regular_identity(&other_status));
    REQUIRE(ld_loop_backing_file_matches_identity(directory, &original_status));
    REQUIRE(ld_loop_backing_file_matches_identity(directory, &alias_status));
    REQUIRE(!ld_loop_backing_file_matches_identity(directory, &other_status));
    /* Exercise both the public probe and the authoritative writable-open
     * refusal against the same inode reached through a hard-link alias. */
    REQUIRE(ld_path_is_mounted(alias));
    LdDevice writable = {.fd = -1};
    errno = 0;
    REQUIRE(ld_device_try_open(alias, true, &writable) != 0);
    REQUIRE(errno == EBUSY);
    REQUIRE(writable.fd < 0);

    REQUIRE(unlink(original) == 0 && unlink(alias) == 0 && unlink(other) == 0);
    REQUIRE(unlink(backing) == 0 && unlink(mountinfo) == 0);
    REQUIRE(rmdir(loop) == 0 && rmdir(directory) == 0);
    puts("mounted-image hard-link identity tests passed");
    return 0;
}
