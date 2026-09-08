/* pps_paths.c -- finding the game on the SD card.
 *
 * MIT licensed. See LICENSE. The search order and its reasoning are inherited
 * from the Osmos port; what differs here is what counts as a game directory
 * and the three writable directories the King engine asks for by name.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "config.h"
#include "pps_paths.h"

static char root[FS_MAX_PATH];
static char so_path[FS_MAX_PATH];
static char assets_root[FS_MAX_PATH];
static char home_dir[FS_MAX_PATH];
static char cache_dir[FS_MAX_PATH];
static char shared_dir[FS_MAX_PATH];
static char uuid[40];
/* Big enough for the prose plus a full FS_MAX_PATH path; a smaller buffer
 * truncated the path in exactly the message whose whole job is to show it. */
static char path_err[FS_MAX_PATH + 640];

#define SD_SWITCH "sdmc:/switch"
#define GAME_SO   "/libpapapearsaga.so"

/* snprintf truncates silently, and a shortened path surfaces much later as an
 * unexplained "file not found". Fail loudly at composition time instead. */
static void join(char *out, size_t cap, const char *dir, const char *leaf) {
  const int n = snprintf(out, cap, "%s%s", dir, leaf);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = 0;
    LOGE("path too long: %s%s", dir, leaf);
  }
}

/* A folder qualifies as the game directory only if it holds BOTH halves of
 * what prepare_game.sh produces. Accepting a folder with the library and no
 * assets would turn "assets are missing" into an asset-manager fault deep
 * inside engine startup, which is a much worse error to be handed.
 *
 * The assets probe is res_output/ rather than assets/ itself, because an empty
 * assets/ directory is a real and common outcome of a half-finished copy. */
static int looks_like_game_dir(const char *dir) {
  char probe[FS_MAX_PATH];
  struct stat st;

  join(probe, sizeof(probe), dir, GAME_SO);
  if (!probe[0] || stat(probe, &st) != 0) return 0;

  join(probe, sizeof(probe), dir, "/assets/res_output");
  if (!probe[0] || stat(probe, &st) != 0) return 0;

  return 1;
}

/* A short record of where we looked, replayed into debug.log once logging is
 * up. pps_paths_init has to run before log_init -- the log lives in the
 * directory this function is trying to find -- so it cannot log as it goes. */
#define SEARCH_LOG_CAP 1024
static char search_log[SEARCH_LOG_CAP];

static void note(const char *fmt, ...) {
  const size_t used = strlen(search_log);
  va_list ap;
  if (used + 2 >= SEARCH_LOG_CAP) return;
  va_start(ap, fmt);
  vsnprintf(search_log + used, SEARCH_LOG_CAP - used, fmt, ap);
  va_end(ap);
}

/* strcasestr is not in newlib; lowercase by hand. Matching on either half of
 * the name means "papapear", "PapaPear", "pear_saga" and "papa" all win. */
static int name_mentions_game(const char *name) {
  char lower[64];
  size_t i = 0;
  for (; name[i] && i < sizeof(lower) - 1; i++) {
    const char c = name[i];
    lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  lower[i] = 0;
  return strstr(lower, "papa") != NULL || strstr(lower, "pear") != NULL;
}

/* One directory level. depth 0 = the children of `base`, depth 1 = recurse
 * into each child too. `named_only` selects the first pass. */
static int scan_level(const char *base, int depth, int named_only,
                      char *out, size_t cap) {
  DIR *d = opendir(base);
  struct dirent *e;
  int looked = 0;
  if (!d) return 0;

  while ((e = readdir(d)) != NULL && looked < 256) {
    char cand[FS_MAX_PATH];
    int n;
    if (e->d_name[0] == '.') continue;
    looked++;

    n = snprintf(cand, sizeof(cand), "%s/%s", base, e->d_name);
    if (n < 0 || (size_t)n >= sizeof(cand)) continue;

    if ((!named_only || name_mentions_game(e->d_name)) &&
        looks_like_game_dir(cand)) {
      closedir(d);
      snprintf(out, cap, "%s", cand);
      return 1;
    }

    if (depth > 0 && scan_level(cand, depth - 1, named_only, out, cap)) {
      closedir(d);
      return 1;
    }
  }
  closedir(d);
  return 0;
}

/* Two passes, and the order is deliberate: a folder whose name mentions the
 * game wins over one that merely happens to contain the files. Without that,
 * the choice among several candidates would depend on readdir order -- which
 * is filesystem order -- so the game could start picking a different folder
 * after an unrelated file was added elsewhere. */
static int scan_switch_dir(char *out, size_t cap) {
  if (scan_level(SD_SWITCH, 1, 1, out, cap)) { note("  found (named) %s\n", out); return 1; }
  if (scan_level(SD_SWITCH, 1, 0, out, cap)) { note("  found %s\n", out); return 1; }
  return 0;
}

void pps_paths_init(void) {
  extern int __system_argc;
  extern char **__system_argv;
  size_t i;

  root[0] = 0;
  search_log[0] = 0;

  /* --- 1. beside the .nro --- */
  if (__system_argc > 0 && __system_argv && __system_argv[0]) {
    char launch[FS_MAX_PATH];
    char *slash;
    snprintf(launch, sizeof(launch), "%s", __system_argv[0]);

    /* Some launchers hand over a bare filename with no directory, and some
     * omit the sdmc: prefix. Neither is usable as-is. */
    slash = strrchr(launch, '/');
    if (slash) {
      *slash = 0;
      if (strncmp(launch, "sdmc:", 5) != 0 && launch[0] == '/')
        join(root, sizeof(root), "sdmc:", launch);
      else
        snprintf(root, sizeof(root), "%s", launch);

      note("  argv[0] -> %s\n", root);
      if (!looks_like_game_dir(root)) {
        note("    (no libpapapearsaga.so + assets/res_output there)\n");
        root[0] = 0;
      }
    } else {
      note("  argv[0] had no directory: %s\n", launch);
    }
  } else {
    note("  no argv[0] from the launcher\n");
  }

  /* --- 2. one or two levels under sdmc:/switch --- */
  if (!root[0]) {
    char found[FS_MAX_PATH];
    note("  scanning %s/*/ and %s/*/*/\n", SD_SWITCH, SD_SWITCH);
    if (scan_switch_dir(found, sizeof(found)))
      snprintf(root, sizeof(root), "%s", found);
  }

  /* --- 3. last resorts --- */
  if (!root[0]) {
    static const char *const fallbacks[] = {
      "sdmc:/switch/papapear", "sdmc:/switch/papapearsaga",
      "sdmc:/switch", "sdmc:/papapear",
    };
    for (i = 0; i < sizeof(fallbacks) / sizeof(*fallbacks); i++) {
      if (looks_like_game_dir(fallbacks[i])) {
        snprintf(root, sizeof(root), "%s", fallbacks[i]);
        note("  found at %s\n", fallbacks[i]);
        break;
      }
    }
  }

  /* Nothing found. Keep the most likely location so the error screen can name
   * a concrete path rather than an empty string. */
  if (!root[0]) {
    snprintf(root, sizeof(root), "sdmc:/switch/papapear");
    note("  nothing found; defaulting to %s\n", root);
  }

  join(so_path,     sizeof(so_path),     root, GAME_SO);
  join(assets_root, sizeof(assets_root), root, "/assets");

  /* Trailing slashes are load-bearing -- see pps_paths.h. */
  join(home_dir,   sizeof(home_dir),   root, "/storage/");
  join(cache_dir,  sizeof(cache_dir),  root, "/cache/");
  join(shared_dir, sizeof(shared_dir), root, "/shared/");
}

/* The engine assumes its home directory already exists: FileSystem returned a
 * path from Activity.getDir(), which Android creates on demand. Nothing here
 * does that for us, so the first save would fail on a missing directory. */
void pps_paths_make_dirs(void) {
  char tmp[FS_MAX_PATH];
  const char *dirs[3];
  size_t i;
  dirs[0] = home_dir; dirs[1] = cache_dir; dirs[2] = shared_dir;

  for (i = 0; i < 3; i++) {
    size_t n;
    snprintf(tmp, sizeof(tmp), "%s", dirs[i]);
    n = strlen(tmp);
    /* mkdir on a path with a trailing slash is fine on newlib, but strip it
     * anyway so the failure message names the directory rather than a path
     * that looks malformed. */
    while (n && tmp[n - 1] == '/') tmp[--n] = 0;
    if (!n) continue;
    if (mkdir(tmp, 0777) != 0) {
      struct stat st;
      if (stat(tmp, &st) != 0)
        LOGE("could not create %s", tmp);
    }
  }
}

void pps_paths_log_search(void) {
  LOGB("game directory: %s", root);
  LOGB("asset root:     %s", assets_root);
  LOGB("home (saves):   %s", home_dir);
  LOGI("search:\n%s", search_log[0] ? search_log : "  (none)");
}

int pps_paths_check(void) {
  struct stat st;
  char probe[FS_MAX_PATH];

  if (stat(so_path, &st) != 0) {
    snprintf(path_err, sizeof(path_err),
             "libpapapearsaga.so not found.\n\n"
             "  Looked in: %s\n"
             "  ...and in every folder one or two levels under sdmc:/switch/\n\n"
             "Extract lib/arm64-v8a/libpapapearsaga.so from your own copy of\n"
             "the Papa Pear Saga APK and put it, plus the assets/ folder,\n"
             "next to the .nro. tools/prepare_game.sh does both.", root);
    return 0;
  }

  /* Probe a file the engine itself opens through the asset manager, rather
   * than merely checking that some assets folder exists. ff-package-bootstrap
   * is the first thing the FF resource system reads, so its absence is the
   * earliest honest signal that the copy is incomplete. */
  join(probe, sizeof(probe), assets_root,
       "/res_output/ff-system-package/ff-package-bootstrap.xml");
  if (!probe[0] || stat(probe, &st) != 0) {
    snprintf(path_err, sizeof(path_err),
             "Game assets are incomplete.\n\n"
             "  Asset root: %s\n"
             "  Missing:    res_output/ff-system-package/ff-package-bootstrap.xml\n\n"
             "The library was found, but the engine cannot see its assets.\n"
             "Copy the whole assets/ folder out of your APK next to it --\n"
             "keep the res_output/ folder inside it exactly as it came.\n"
             "tools/prepare_game.sh does both.", assets_root);
    return 0;
  }
  return 1;
}

const char *pps_paths_error(void)  { return path_err; }
const char *pps_root(void)         { return root; }
const char *pps_so_path(void)      { return so_path; }
const char *pps_assets_root(void)  { return assets_root; }
const char *pps_home_dir(void)     { return home_dir; }
const char *pps_cache_dir(void)    { return cache_dir; }
const char *pps_shared_dir(void)   { return shared_dir; }

/* Device.getDeviceId() and UuidGenerator.getUuid() feed the engine's
 * per-install id. It is used for local bookkeeping only -- there is no server
 * to report it to -- so any stable value works. Generate once and keep it, so
 * progress stays associated with the same install. */
const char *pps_device_uuid(void) {
  char p[FS_MAX_PATH];
  FILE *f;
  u64 a, b = 0;

  if (uuid[0]) return uuid;

  join(p, sizeof(p), root, "/uuid.txt");

  f = fopen(p, "r");
  if (f) {
    if (fgets(uuid, sizeof(uuid), f)) {
      char *nl = strpbrk(uuid, "\r\n");
      if (nl) *nl = 0;
    }
    fclose(f);
    if (uuid[0]) return uuid;
  }

  /* randomGet is libnx's own CSPRNG and needs no service brought up first;
   * csrngGetRandomBytes would require csrngInitialize, which the default
   * libnx init does not do. */
  a = armGetSystemTick();
  randomGet(&b, sizeof(b));
  snprintf(uuid, sizeof(uuid), "%016llx%016llx",
           (unsigned long long)a, (unsigned long long)b);

  f = fopen(p, "w");
  if (f) { fprintf(f, "%s\n", uuid); fclose(f); }
  return uuid;
}
