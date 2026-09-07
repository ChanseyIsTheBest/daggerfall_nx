/* nx_data_root.c -- see nx_data_root.h for the why and the resolution order.
 *
 * Adopted from clonehero_nx's nx_data_root.c; steps 4 and 5 (scan sdmc:/ one
 * and two levels deep) are added here so the folder can live anywhere on the
 * card, not only under sdmc:/switch/.
 */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "config.h"
#include "nx_data_root.h"

char g_data_root[512];
char g_log_path[576];
char g_data_root_how[512];

#ifndef DATA_ROOT_DEFAULT
#define DATA_ROOT_DEFAULT "sdmc:/switch/" GAME_FOLDER
#endif
#define SWITCH_DIR  "sdmc:/switch"
#define SD_ROOT     "sdmc:/"

/* Bounds on the scan. A card can have a lot of folders; these keep a bad
 * layout from turning into a long stall before the error appears. */
#define MAX_ENTRIES_PER_DIR 512
#define MAX_SUBDIRS_DEEP    128

/* A directory counts as the game folder only if libmain.so is in it. Cheap,
 * and it is exactly the file whose absence started this. */
static int looks_like_root(const char *dir) {
  char p[640];
  struct stat st;
  if (!dir || !*dir) return 0;
  snprintf(p, sizeof p, "%s/libmain.so", dir);
  return stat(p, &st) == 0 && st.st_size > 0;
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void adopt(const char *dir, const char *how) {
  snprintf(g_data_root, sizeof g_data_root, "%s", dir);
  snprintf(g_log_path,  sizeof g_log_path,  "%s/debug.log", g_data_root);
  snprintf(g_data_root_how, sizeof g_data_root_how, "%s", how);
}

/* Scan one directory level for a child that looks like the game root. */
static int scan_level(const char *base, char *out, size_t outsz, char *how, size_t howsz) {
  DIR *d = opendir(base);
  if (!d) return 0;
  struct dirent *de;
  int looked = 0;
  while ((de = readdir(d)) != NULL && looked < MAX_ENTRIES_PER_DIR) {
    if (de->d_name[0] == '.') continue;
    looked++;
    char cand[512];
    /* base may or may not end in '/' (SD_ROOT does, SWITCH_DIR does not).
     * Joining blindly gives "sdmc://name", which then leaks into g_data_root
     * and into every strncmp() that compares a path against it. */
    size_t bl = strlen(base);
    int trailing = (bl && base[bl - 1] == '/');
    snprintf(cand, sizeof cand, trailing ? "%s%s" : "%s/%s", base, de->d_name);
    if (looks_like_root(cand)) {
      snprintf(out, outsz, "%s", cand);
      snprintf(how, howsz, "found by scanning %s (folder '%s')", base, de->d_name);
      closedir(d);
      return 1;
    }
  }
  closedir(d);
  return 0;
}

void nx_resolve_data_root(int argc, char *argv[]) {
  char cand[512], how[512];

  /* ---- 1. the directory the .nro was launched from ---------------------- */
  if (argc >= 1 && argv && argv[0] && argv[0][0]) {
    const char *a0 = argv[0];
    /* hbmenu normally gives "sdmc:/switch/<dir>/<name>.nro". Some launchers
     * hand over a bare "/switch/..." with no device prefix; normalise that. */
    if (!strchr(a0, ':') && a0[0] == '/')
      snprintf(cand, sizeof cand, "sdmc:%s", a0);
    else
      snprintf(cand, sizeof cand, "%s", a0);

    char *slash = strrchr(cand, '/');
    if (slash && slash != cand) {
      *slash = '\0';                       /* strip "/<name>.nro"            */
      if (looks_like_root(cand)) {
        adopt(cand, "from argv[0] (.nro location)");
        return;
      }
    }
  }

  /* ---- 2. compile-time default ------------------------------------------ */
  if (looks_like_root(DATA_ROOT_DEFAULT)) {
    adopt(DATA_ROOT_DEFAULT, "compile-time default");
    return;
  }

  /* ---- 3. one level under sdmc:/switch/ --------------------------------- */
  if (scan_level(SWITCH_DIR, cand, sizeof cand, how, sizeof how)) {
    adopt(cand, how);
    return;
  }

  /* ---- 4. one level under sdmc:/ ---------------------------------------- */
  if (scan_level(SD_ROOT, cand, sizeof cand, how, sizeof how)) {
    adopt(cand, how);
    return;
  }

  /* ---- 5. two levels under sdmc:/ --------------------------------------- *
   * For cards organised as sdmc:/games/daggerfall/ or similar. Skips
   * sdmc:/switch (already covered) and the obvious system folders, and stops
   * after MAX_SUBDIRS_DEEP directories so a huge card cannot stall boot. */
  {
    DIR *d = opendir(SD_ROOT);
    if (d) {
      struct dirent *de;
      int looked = 0;
      while ((de = readdir(d)) != NULL && looked < MAX_SUBDIRS_DEEP) {
        if (de->d_name[0] == '.') continue;
        if (!strcasecmp(de->d_name, "switch"))   continue;  /* done in step 3 */
        if (!strcasecmp(de->d_name, "Nintendo")) continue;  /* huge, never it */
        if (!strcasecmp(de->d_name, "atmosphere")) continue;
        if (!strcasecmp(de->d_name, "emuMMC"))   continue;
        char sub[512];
        snprintf(sub, sizeof sub, "%s%s", SD_ROOT, de->d_name);
        if (!is_dir(sub)) continue;
        looked++;
        if (scan_level(sub, cand, sizeof cand, how, sizeof how)) {
          closedir(d);
          adopt(cand, how);
          return;
        }
      }
      closedir(d);
    }
  }

  /* ---- 6. nothing validated; keep the default so errors read sensibly ---- */
  adopt(DATA_ROOT_DEFAULT,
        "NOT FOUND -- no folder on the card contains libmain.so; "
        "falling back to the compile-time default");
}

const char *nx_path(const char *sub) {
  static char buf[8][768];
  static unsigned n = 0;
  char *b = buf[n++ & 7];
  snprintf(b, sizeof buf[0], "%s%s", g_data_root, sub ? sub : "");
  return b;
}

/* Daggerfall needs the original game data (the ARENA2 folder, freeware from
 * Bethesda). Case varies between distributions -- the folder is "ARENA2" in
 * the original release and "arena2" in most repacks -- and the Switch SD
 * filesystem is case-sensitive through this path, so check both. */
int nx_have_arena2(void) {
  static const char *names[] = { "/arena2", "/ARENA2", "/Arena2" };
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
    if (is_dir(nx_path(names[i]))) return 1;
  return 0;
}
