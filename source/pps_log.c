/* pps_log.c -- the engine's own logging, mirrored into debug.log.
 *
 * MIT licensed. See LICENSE.
 *
 * WHY THIS IS WORTH HAVING
 * ------------------------
 * King's engine is chatty in a useful way. It reports its own failures --
 * missing assets, shaders that would not compile, scenes whose XML did not
 * parse, resource packages it could not mount -- through __android_log_print
 * and nowhere else. Nothing in that path throws, returns an error code, or
 * crashes: a scene that fails to load is simply not drawn.
 *
 * So on a black screen with a healthy frame counter, this file is the only
 * thing that will tell you why. That is a different situation from Osmos,
 * where the equivalent tracing was expensive per-frame instrumentation; here
 * it is a few hundred lines across an entire boot and costs nothing.
 *
 * It still follows DEBUG_LOG, because a normal session should write nothing.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "pps_log.h"
#include "util.h"   /* debugLogNote is declared there */

/* Android log priorities. The engine uses INFO, WARN and ERROR; the com.abm
 * and com.king Logging classes map onto the same set. */
enum {
  ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE,
  ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR,
  ANDROID_LOG_FATAL, ANDROID_LOG_SILENT
};

static char level_of(int prio) {
  switch (prio) {
    case ANDROID_LOG_ERROR:
    case ANDROID_LOG_FATAL: return 'E';
    case ANDROID_LOG_WARN:  return 'W';
    default:                return 'I';
  }
}

/* Errors and warnings from the engine are promoted to LOGE/LOGW, which are
 * compiled in even when DEBUG_LOG is off. That is the whole point: a build
 * that writes nothing on a good run still leaves the engine's own complaint
 * behind on a bad one. */
static void emit(int prio, const char *tag, const char *text) {
  const char level = level_of(prio);
  const char *t = tag ? tag : "?";
  const char *m = text ? text : "";

  if (level == 'E')      LOGE("[%s] %s", t, m);
  else if (level == 'W') LOGW("[%s] %s", t, m);
  else                   LOGB("[%s] %s", t, m);
}

int pps_log_print(int prio, const char *tag, const char *fmt, ...) {
#if PPS_MIRROR_ENGINE_LOG
  char buf[1024];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt ? fmt : "", va);
  va_end(va);
  emit(prio, tag, buf);
#else
  /* Errors still get through with mirroring off -- they are rare and they are
   * the reason anyone opens the file. */
  if (prio >= ANDROID_LOG_ERROR) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", va);
    va_end(va);
    emit(prio, tag, buf);
  } else {
    (void)tag; (void)fmt;
  }
#endif
  return 0;
}

int pps_log_write_ndk(int prio, const char *tag, const char *text) {
#if PPS_MIRROR_ENGINE_LOG
  emit(prio, tag, text);
#else
  if (prio >= ANDROID_LOG_ERROR) emit(prio, tag, text);
  else { (void)tag; (void)text; }
#endif
  return 0;
}

/* bionic's android_set_abort_message stashes a string that the crash reporter
 * would have picked up. Nothing here reads it back, but it is often the most
 * specific thing the engine ever says -- an assertion message names the file
 * and line -- so it is logged at error level unconditionally. */
void pps_set_abort_message(const char *msg) {
  LOGE("abort: %s", msg ? msg : "(null)");
}

/* ------------------------------------------------------------------ */
/* stdout                                                              */
/* ------------------------------------------------------------------ */

/* Not actually discarded, despite the name -- kept from the reference ports
 * so the resolution is recognisable. The engine's printf output goes to
 * debug.log, because the alternatives are worse: the console devoptab either
 * does not exist in a graphical homebrew, or it is the framebuffer that EGL
 * has taken, and writing to that under a live GL context can lose the display
 * outright.
 *
 * These are cheap. With DEBUG_LOG off, log_write's lazy open means nothing is
 * created unless something has already gone wrong. */
int pps_vprintf_discard(const char *fmt, va_list ap) {
  char buf[1024];
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  /* Trim the trailing newline: log_write adds its own, and the engine's
   * printf calls are inconsistent about including one. */
  size_t len = strlen(buf);
  while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
  if (len) LOGB("[stdout] %s", buf);
  return n;
}

int pps_printf_discard(const char *fmt, ...) {
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = pps_vprintf_discard(fmt, ap);
  va_end(ap);
  return n;
}

int pps_puts_discard(const char *s) {
  if (s && *s) LOGB("[stdout] %s", s);
  return 0;
}

/* ------------------------------------------------------------------ */
/* debugLogNote                                                        */
/* ------------------------------------------------------------------ */

/* Declared in util.h and called from opensles.c and mp3_decode.c, both of
 * which are vendored unmodified. In the Sonic Jump port the body lived in
 * sj_glue.c, which this tree does not carry -- so it was declared, called from
 * two files, and defined nowhere.
 *
 * That is exactly the class of failure tools/check_links.py exists to catch,
 * and it did not: the check classified symbols as "ours" by NAME PATTERN, and
 * debugLogNote matches none of the prefixes or suffixes the port uses, so it
 * was assumed to come from newlib. The check has been changed to stop guessing
 * from names; see the note at the top of that file.
 *
 * Routed to debug.log rather than the console. opensles.c uses this for its
 * device-open reporting, which is precisely what you want to read when audio
 * is silent, and printing it to a console that EGL owns would be either
 * invisible or destructive. */
int debugLogNote(const char *text, ...) {
  char buf[512];
  va_list ap;
  int n;
  size_t len;

  va_start(ap, text);
  n = vsnprintf(buf, sizeof(buf), text, ap);
  va_end(ap);

  /* These call sites embed their own trailing newline; log_write adds one. */
  len = strlen(buf);
  while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
  if (len) LOGB("%s", buf);
  return n;
}
