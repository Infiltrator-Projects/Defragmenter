/* SPDX-License-Identifier: GPL-3.0-or-later
 * OFS uses the same strict Common V2 dispatch as every other provider. */
#include "common_v2_worker.h"
int main(int argc,char **argv)
{
    static const char *const formats[]={"ofs"};
    return ifs_v2_worker_dispatch(argc,argv,formats,1u);
}
