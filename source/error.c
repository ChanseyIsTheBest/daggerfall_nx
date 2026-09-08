/* error.c -- fatal errors and the startup status screen.
 *
 * MIT licensed. See LICENSE.
 *
 * This replaces the reference ports' error.c rather than reusing it. Theirs
 * depends on sj_log.c, which repoints devoptab_list[STD_OUT] at a file so that
 * a stray printf anywhere in the port cannot fault on libnx's console
 * renderer, and then needs con_write_direct() to reach the screen again. This
 * port does not redirect stdout, so plain printf after consoleInit is correct
 * and the whole indirection is unnecessary.
 *
 * so_util.c calls fatal_error() in twelve places, so the signature has to
 * match exactly: printf-style, noreturn.
 *
 * One real constraint: a console and an EGL surface cannot both own the
 * default window. Anything here that draws a console must therefore run either
 * before EGL comes up or after it has been torn down -- see gfx_release()
 * below, which main.c installs so this file can do that without knowing about
 * EGL.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "error.h"

static int status_active;
static void (*gfx_release_hook)(void);

void error_set_gfx_release(void (*fn)(void)) { gfx_release_hook = fn; }

/* consoleInit() takes the default window. If EGL currently holds it, hand it
 * back first, or the console draws into nothing and the message is lost --
 * which is exactly the situation where the message matters most. */
static void take_console(void) {
  /* NOT one-shot. This used to clear the hook after calling it, so the FIRST
   * take_console consumed it -- and the first is the startup status screen,
   * which runs before EGL even exists. Every later error screen, including the
   * fatal ones that matter most, would then have drawn into a window EGL still
   * owned. egl_stop() is idempotent, so calling it more than once is free. */
  if (gfx_release_hook) gfx_release_hook();
  consoleInit(NULL);
}

static void wait_for_plus(void) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
    svcSleepThread(16000000ull);
  }
}

void startup_status_update(const char *message) {
  if (!status_active) return;
  printf("\x1b[2J\x1b[H\n\n  papapear_nx\n\n  %s\n\n  Please wait...\n",
         message ? message : "");
  consoleUpdate(NULL);
}

void startup_status_begin(const char *message) {
  if (!status_active) { take_console(); status_active = 1; }
  startup_status_update(message);
}

void startup_status_end(void) {
  if (!status_active) return;
  consoleExit(NULL);
  status_active = 0;
}

void fatal_error(const char *fmt, ...) {
  char message[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);

  /* Get it into the log before taking over the screen: the lines above the
   * failure are usually what explain it, and a log that does not survive the
   * crash it describes is worse than no log. */
  LOGE("fatal: %s", message);
  log_close();

  if (status_active) startup_status_end();
  take_console();

  printf("\x1b[2J\x1b[H\n\n  papapear_nx\n\n  Fatal error:\n\n  %s\n\n"
         "  Press + to exit.\n", message);
  consoleUpdate(NULL);

  wait_for_plus();
  consoleExit(NULL);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(1);
}

/* Non-fatal variant used for the missing-files case, which is the most common
 * first-run failure and deserves a readable explanation rather than a crash
 * dump. Returns so main() can exit cleanly. */
void error_screen(const char *msg) {
  LOGE("startup: %s", msg ? msg : "?");

  if (status_active) startup_status_end();
  take_console();

  printf("\x1b[2J\x1b[H\n  papapear_nx\n  --------\n\n  %s\n\n  Press + to exit.\n",
         msg ? msg : "unknown error");
  consoleUpdate(NULL);

  wait_for_plus();
  consoleExit(NULL);
}
