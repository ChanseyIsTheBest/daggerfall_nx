/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "nx_data_root.h"  /* g_log_path: resolved at runtime, see main() */

// Thread-safe, file-only logger. We open+flush+close on every call so the last
// lines survive a hard crash, and serialise with a mutex because the engine
// logs from several worker threads. No nxlink/socket: this must work on bare
// hardware. The log lands in the game dir (main() chdir()s there at startup).
#if DEBUG_LOG
static Mutex g_log_lock; // libnx Mutex: 0 == unlocked, no init needed
#endif

/* Force the log out. Called by the exception handler (before and after it
 * writes the dump) and at shutdown, so the timer-based flush above can never
 * cost us the one log that matters. */
void debug_log_flush(void) {
#if DEBUG_LOG
  extern FILE *debug_log_file(void);
  FILE *f = debug_log_file();
  if (f) { mutexLock(&g_log_lock); fflush(f); mutexUnlock(&g_log_lock); }
#endif
}

static FILE *g_log_f = NULL;
FILE *debug_log_file(void) { return g_log_f; }

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  FILE *f = g_log_f;
  va_list list;
  mutexLock(&g_log_lock);
  if (!f) f = g_log_f = fopen(g_log_path, "a");  // open once, keep open
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    /* DO NOT fflush() every line. It used to, "so we don't lose it if we crash
     * next" -- but each flush is a synchronous SD-card write, and a normal run
     * emits over eleven thousand lines. Daggerfall opens BSA files constantly,
     * so those writes land in the middle of gameplay and show up as stutter.
     *
     * Flush on a timer instead. At most one write every LOG_FLUSH_MS, so a
     * burst of a hundred lines costs one flush rather than a hundred, and the
     * most that can be lost to a hard hang is that window. Nothing is lost to a
     * CRASH: the exception handler calls debug_log_flush() before it prints,
     * and again after. */
    static uint64_t last_flush;
    const uint64_t now = armGetSystemTick();
    if (now - last_flush >= armNsToTicks((uint64_t)LOG_FLUSH_MS * 1000000ull)) {
      last_flush = now;
      fflush(f);
    }
  }
  mutexUnlock(&g_log_lock);
#else
  (void)text;
#endif
  return 0;
}

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
