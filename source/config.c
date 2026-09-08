/* config.c -- the handful of settings that survive, plus logging.
 *
 * MIT licensed. See LICENSE.
 *
 * Everything else is a constant in config.h. A setting is a place to put a
 * wrong value, and the render size, the device layout and the touch mapping
 * are decisions rather than preferences -- the port either has them right or
 * it is broken, and making them configurable only hides that.
 *
 * What remains is what only the player can answer.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "config.h"
#include "pps_paths.h"
#include "pps_io.h"
#include "error.h"

/* See the note in config.h: opensles.c reads config.decode_stream_audio. */
PpsCompatConfig config = { 1 };

static char c_language[16] = "auto";
static char c_country[8]   = "auto";
static int  c_vsync        = 1;
static int  c_fps          = PPS_TARGET_FPS_DEFAULT;
static int  c_purchases    = 1;
static float c_volume      = 1.0f;

/* One block per setting, so a config.txt written by an older build gains the
 * settings it is missing without disturbing the choices already in it. The
 * file is only created when absent, so otherwise a new option would be
 * invisible forever to anyone who had run the port before. */
typedef struct { const char *key, *block; } CfgBlock;

static const CfgBlock CFG_BLOCKS[] = {
  { "language",
    "# language: which localisation the game loads. \"auto\" follows the console.\n"
    "#   en de es fr it nl pt ru sv da no fi tr ja ko zh ar\n"
    "language = auto\n\n" },

  { "country",
    "# country: the region code reported to the engine (DeviceLocale.getCountryCode).\n"
    "# \"auto\" derives it from the console's region. Only affects currency and\n"
    "# copy in a few places; the store is local here regardless.\n"
    "country = auto\n\n" },

  { "target_fps",
    "# target_fps: what the engine is told to aim for. 60 or 30.\n"
    "#\n"
    "# On Android the Java renderer slept to hold this; here the EGL swap\n"
    "# interval does the pacing, so this only sets what the engine believes.\n"
    "# Setting 30 makes the engine's own animation timing halve to match.\n"
    "target_fps = 60\n\n" },

  { "vsync",
    "# vsync: on or off. Off is only useful for measuring the frame cost.\n"
    "vsync = on\n\n" },

  { "volume",
    "# volume: master output scale, 0.0 to 1.0.\n"
    "volume = 1.0\n\n" },

  { "purchases",
    "# purchases: whether to read purchases.txt at startup.\n"
    "#\n"
    "# Papa Pear Saga sells gold bars through Google Play. There is no Play\n"
    "# Store here to ask what you own, so the port reads purchases.txt and\n"
    "# delivers those entitlements through the engine's own restore path.\n"
    "# See purchases.txt for the detail. Set \"off\" to skip it entirely.\n"
    "purchases = on\n\n" },
};

#define CFG_NBLOCKS ((int)(sizeof(CFG_BLOCKS) / sizeof(*CFG_BLOCKS)))

static const char *CFG_HEADER =
  "# papapear_nx configuration.\n"
  "#\n"
  "# Most of the port is hardcoded on purpose. These are the settings only you\n"
  "# can answer, so they live here.\n"
  "\n";

static void trim(char *s) {
  char *p = s;
  size_t n;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int truthy(const char *v) {
  return !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true");
}

void cfg_load(void) {
  char path[FS_MAX_PATH];
  int seen[CFG_NBLOCKS];
  int seen_dead_key = 0, missing = 0, i;
  char line[256];
  FILE *f;

  snprintf(path, sizeof(path), "%s/config.txt", pps_root());
  memset(seen, 0, sizeof(seen));

  f = fopen_locked(path, "r");
  if (!f) {
    f = fopen_locked(path, "w");
    if (f) {
      fputs(CFG_HEADER, f);
      for (i = 0; i < CFG_NBLOCKS; i++) fputs(CFG_BLOCKS[i].block, f);
      fclose_locked(f);
    }
    f = fopen_locked(path, "r");
    if (!f) return;
  }

  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    char *eq, *key, *val;
    if (hash) *hash = 0;
    eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    key = line; val = eq + 1;
    trim(key); trim(val);
    if (!*key || !*val) continue;

    for (i = 0; i < CFG_NBLOCKS; i++)
      if (!strcmp(key, CFG_BLOCKS[i].key)) seen[i] = 1;

    if      (!strcmp(key, "language"))   snprintf(c_language, sizeof(c_language), "%s", val);
    else if (!strcmp(key, "country"))    snprintf(c_country, sizeof(c_country), "%s", val);
    else if (!strcmp(key, "vsync"))      c_vsync = truthy(val);
    else if (!strcmp(key, "purchases"))  c_purchases = truthy(val);
    else if (!strcmp(key, "volume"))     c_volume = strtof(val, NULL);
    else if (!strcmp(key, "target_fps")) c_fps = (int)strtol(val, NULL, 10);
    else                                 seen_dead_key = 1;
  }
  fclose_locked(f);

  for (i = 0; i < CFG_NBLOCKS; i++) if (!seen[i]) missing++;
  if (missing) {
    FILE *a = fopen_locked(path, "a");
    if (a) {
      for (i = 0; i < CFG_NBLOCKS; i++)
        if (!seen[i]) fputs(CFG_BLOCKS[i].block, a);
      fclose_locked(a);
    }
    LOGW("config.txt was missing %d setting(s); appended with defaults.", missing);
  }

  if (c_volume < 0.0f) c_volume = 0.0f;
  if (c_volume > 1.0f) c_volume = 1.0f;
  /* The engine derives its animation step from this, so a nonsense value here
   * does not merely pace badly -- it changes gameplay speed. */
  if (c_fps != 30 && c_fps != 60) {
    LOGW("target_fps must be 30 or 60; %d ignored, using %d.", c_fps, PPS_TARGET_FPS_DEFAULT);
    c_fps = PPS_TARGET_FPS_DEFAULT;
  }

  if (seen_dead_key)
    LOGW("config.txt has settings that are no longer configurable; ignored.");
}

/* libnx's default init does not bring up the 'set' service, so it has to be
 * opened and closed around the query. */
static SetLanguage system_language(void) {
  u64 lc = 0;
  SetLanguage sl = SetLanguage_ENUS;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&lc)))
      setMakeLanguage(lc, &sl);
    setExit();
  }
  return sl;
}

/* The engine names its localisation files with these codes -- taken from the
 * strings_<code>.csv set actually present in assets/res_output/localization,
 * not from the IETF tags libnx reports. pt_BR and zh_CN exist as separate
 * files but the base codes are the safe default. */
const char *cfg_language(void) {
  if (strcmp(c_language, "auto") != 0) return c_language;
  switch (system_language()) {
    case SetLanguage_JA:     return "ja";
    case SetLanguage_FR:
    case SetLanguage_FRCA:   return "fr";
    case SetLanguage_DE:     return "de";
    case SetLanguage_IT:     return "it";
    case SetLanguage_ES:
    case SetLanguage_ES419:  return "es";
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS:
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT: return "zh";
    case SetLanguage_KO:     return "ko";
    case SetLanguage_RU:     return "ru";
    case SetLanguage_NL:     return "nl";
    case SetLanguage_PT:
    case SetLanguage_PTBR:   return "pt";
    default:                 return "en";
  }
}

const char *cfg_country(void) {
  if (strcmp(c_country, "auto") != 0) return c_country;
  switch (system_language()) {
    case SetLanguage_JA:     return "JP";
    case SetLanguage_FR:     return "FR";
    case SetLanguage_FRCA:   return "CA";
    case SetLanguage_DE:     return "DE";
    case SetLanguage_IT:     return "IT";
    case SetLanguage_ES:     return "ES";
    case SetLanguage_ES419:  return "MX";
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS: return "CN";
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT: return "TW";
    case SetLanguage_KO:     return "KR";
    case SetLanguage_RU:     return "RU";
    case SetLanguage_NL:     return "NL";
    case SetLanguage_PTBR:   return "BR";
    case SetLanguage_PT:     return "PT";
    case SetLanguage_ENGB:   return "GB";
    default:                 return "US";
  }
}

int   cfg_vsync(void)             { return c_vsync; }
int   cfg_target_fps(void)        { return c_fps; }
int   cfg_purchases_enabled(void) { return c_purchases; }
float cfg_master_volume(void)     { return c_volume; }

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static FILE *logf;
static int   log_ready;

/* Opened on demand. With DEBUG_LOG off a normal session writes nothing at all
 * and no file is created; the first warning or error opens it, so a session
 * that goes wrong still leaves something to read. */
static void log_open_if_needed(void) {
  char p[FS_MAX_PATH];
  if (log_ready) return;
  log_ready = 1;
  snprintf(p, sizeof(p), "%s/debug.log", pps_root());
  logf = fopen_locked(p, "w");
}

void log_init(void) {
#if DEBUG_LOG
  log_open_if_needed();
#endif
}

/* Flushed per line. A log that does not survive the crash it was describing
 * tells you the last thing that worked, not the first thing that did not. */
void log_write(char level, const char *fmt, ...) {
  va_list va;
  log_open_if_needed();
  if (!logf) return;
  fprintf(logf, "[%c] ", level);
  va_start(va, fmt);
  vfprintf(logf, fmt, va);
  va_end(va);
  fputc('\n', logf);
  fflush(logf);
}

void log_close(void) { if (logf) { fclose_locked(logf); logf = NULL; } }

/* Engine-originated user messages (toasts, URLs it wanted to open). Logged
 * rather than drawn; nothing here is load-bearing for gameplay. */
void overlay_note(const char *text) {
  LOGB("[note] %s", text ? text : "");
}
