/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "error.h"

static int status_active;
static int status_retired;

void startup_status_update(const char *message) {
  if (!status_active) return;
  printf("\x1b[2J\x1b[H\n\n  Jetpack Joyride NX\n\n  %s\n\n  Please wait...", message);
  consoleUpdate(NULL);
}

void startup_status_begin(const char *message) {
  if (!status_active) {
    consoleInit(NULL);
    status_active = 1;
  }
  startup_status_update(message);
}

void startup_status_end(void) {
  if (!status_active) return;
  consoleExit(NULL);
  status_active = 0;
  status_retired = 1;
}

void fatal_error(const char *fmt, ...) {
  char message[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
  if (!status_active && status_retired) {
    extern void NX_NORETURN __libnx_exit(int rc);
    __libnx_exit(1);
  }
  if (!status_active) {
    consoleInit(NULL);
    status_active = 1;
  }
  printf("\x1b[2J\x1b[H\n\n  Jetpack Joyride NX\n\n"
         "  Fatal error:\n\n  %s\n\n"
         "  Press + to exit.\n", message);
  consoleUpdate(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
    svcSleepThread(16000000ull);
  }
  consoleExit(NULL);
  status_active = 0;
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(1);
}
