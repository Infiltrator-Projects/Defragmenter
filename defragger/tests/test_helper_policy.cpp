// SPDX-License-Identifier: GPL-3.0-or-later
#include "helper_policy.hpp"

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool check(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "helper-policy test failed: %s\n", message);
    return false;
}

bool rejects(const std::function<void()>& operation)
{
    try {
        operation();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    using defragger::helper_command;
    bool ok = true;

    const auto mapper = helper_command(
        "mapper",
        {"/dev/sda2", "--fstype", "ext4", "--cells", "4096"},
        1000U);
    ok = check(
             mapper.executable ==
                 "/usr/lib/linux-defragger/linux-defragger-mapper" &&
                 mapper.arguments == std::vector<std::string>{
                     "/dev/sda2", "--fstype", "ext4", "--cells", "4096"},
             "exact read-only mapper contract is accepted") && ok;

    ok = check(
             rejects([] {
                 (void)helper_command(
                     "mapper", {"/tmp/volume.img", "--fstype", "ext4",
                                "--cells", "4096"}, 1000U);
             }),
             "privileged mapper rejects regular-image paths") && ok;
    ok = check(
             rejects([] {
                 (void)helper_command(
                     "mapper", {"/dev/shm/volume.img", "--fstype", "ext4",
                                "--cells", "4096"}, 1000U);
             }),
             "privileged mapper rejects user-writable /dev subtrees") && ok;
    ok = check(
             rejects([] {
                 (void)helper_command(
                     "mapper", {"/dev/../etc/shadow", "--fstype", "ext4",
                                "--cells", "4096"}, 1000U);
             }),
             "privileged mapper rejects non-normalised device paths") && ok;
    ok = check(
             rejects([] {
                 (void)helper_command(
                     "mapper", {"/dev/sda2", "--probe"}, 1000U);
             }),
             "privileged mapper rejects alternate command forms") && ok;
    ok = check(
             rejects([] {
                 (void)helper_command(
                     "mapper", {"/dev/sda2", "--fstype", "ext4;id",
                                "--cells", "4096"}, 1000U);
             }),
             "privileged mapper rejects malformed filesystem identifiers") && ok;

    for (const char* cells : {"0", "1048577", "4096x"}) {
        ok = check(
                 rejects([cells] {
                     (void)helper_command(
                         "mapper", {"/dev/sda2", "--fstype", "ext4",
                                    "--cells", cells}, 1000U);
                 }),
                 "privileged mapper rejects invalid map-cell bounds") && ok;
    }

    const auto unmount = helper_command(
        "udisksctl", {"unmount", "-b", "/dev/sda2"}, 1000U);
    ok = check(
             unmount.executable == "/usr/bin/udisksctl",
             "normalised unmount target is accepted") && ok;
    ok = check(
             rejects([] {
                 (void)helper_command(
                     "udisksctl",
                     {"unmount", "-b", "/dev/shm/fake-device"}, 1000U);
             }),
             "unmount helper rejects user-writable /dev subtrees") && ok;

    return ok ? 0 : 1;
}
