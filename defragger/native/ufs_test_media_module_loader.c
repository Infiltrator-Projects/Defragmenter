/* SPDX-License-Identifier: GPL-3.0-or-later
 * Dispatch exclusively through Common V2, with unique source-owned probing.
 * An older private filesystem symbol is never requested or accepted.
 */
#include "common_v2_worker.h"
int main(int argc,char **argv)
{
    static const char *const formats[]={"ufs2"};
    return ifs_v2_worker_dispatch(argc,argv,formats,
                                  sizeof(formats)/sizeof(formats[0]));
}
