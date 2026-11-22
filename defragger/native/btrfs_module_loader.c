/* SPDX-License-Identifier: GPL-3.0-or-later
 * BTRFS worker: dispatch exclusively through the Common V2 module.
 * There is no compatibility symbol resolution, private ABI or fallback.
 */
#include "common_v2_worker.h"
int main(int argc,char **argv)
{
    static const char *const formats[]={"btrfs"};
    return ifs_v2_worker_dispatch(argc,argv,formats,1u);
}
