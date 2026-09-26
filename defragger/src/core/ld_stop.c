// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_stop.h"
#include "ld_runtime.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    g_stop_requested = 1;
}

void ld_stop_install_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0) ld_die_errno("install SIGINT handler");
    if (sigaction(SIGTERM, &action, NULL) != 0) ld_die_errno("install SIGTERM handler");
}

void ld_stop_report_ready(void) {
    (void)fputs(LD_STOP_READY_MARKER "\n", stdout);
    (void)fflush(stdout);
}

bool ld_stop_requested(void) {
    return g_stop_requested != 0;
}

void ld_stop_clear(void) {
    g_stop_requested = 0;
}
