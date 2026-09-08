/* pps_assets.c -- AAssetManager over a real directory. MIT licensed.
 *
 * This is the file Osmos did not need and this port cannot do without.
 *
 * libpapapearsaga.so imports AAssetManager_fromJava, AAssetManager_open,
 * AAsset_read, AAsset_getLength and AAsset_close, and every asset it loads
 * comes through them -- textures, scene XML, shader blobs, localisation CSV,
 * the FF resource packages. There is no fopen path to fall back on: if this
 * layer is wrong, nothing loads and the engine draws an empty frame without
 * reporting an error.
 *
 * The mapping is simple because the assets are loose files. The engine builds
 * paths under a "content:///res_output/" URI root and hands the part after
 * the scheme to AAssetManager_open, so:
 *
 *     open("res_output/scenes/root.xml")
 *       -> <gamedir>/assets/res_output/scenes/root.xml
 *
 * Nothing is repacked and nothing is converted. The files are read exactly as
 * they came out of the APK.
 *
 * WHY THE WHOLE ASSET IS READ UP FRONT
 * ------------------------------------
 * AAsset_read is a streaming call and the obvious implementation keeps a FILE*
 * open per asset. That is wrong here for a specific reason: the engine opens
 * assets from several threads at once (its resource loader is a worker pool)
 * and holds them open across frames, and devkitPro's newlib has a small,
 * process-wide, not-thread-safe handle table. Bounded buffering trades memory
 * for not running out of descriptors halfway through a level load.
 *
 * The cost is bounded by ASSET_BUFFER_MAX: anything larger streams from a
 * locked FILE* instead, which is the case for a handful of the bigger .ffm
 * models and nothing else.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "pps_assets.h"
#include "pps_paths.h"
#include "pps_io.h"

/* Read assets up to this size into memory on open; stream anything larger.
 * 4 MB covers every file in the shipped asset set except a few models. */
#define ASSET_BUFFER_MAX (4 * 1024 * 1024)

typedef struct {
  uint32_t magic;
  unsigned char *data;   /* buffered contents, or NULL when streaming */
  FILE *fp;              /* streaming handle, or NULL when buffered   */
  long  size;
  long  pos;
} Asset;

#define ASSET_MAGIC 0x50505341u   /* 'PPSA' */

/* A single opaque handle stands in for the AAssetManager. The engine gets it
 * from AAssetManager_fromJava(env, javaAssetManagerObject) and only ever
 * passes it back to us, so it needs to be a stable non-null pointer and
 * nothing more. */
static int g_manager_token;

/* Opens that found nothing. A handful is normal -- the engine probes for
 * optional assets. Thousands means the asset root is wrong, and telling those
 * two apart from a log is the entire reason this is counted. */
static int  g_misses;
static char g_first_miss[128];

/* Reject anything that could climb out of the asset root. The engine never
 * does this, but the paths are assembled from strings that come out of scene
 * XML, and an asset root is exactly the sort of thing that should not be
 * escapable by a malformed file. */
static int path_is_safe(const char *p) {
  if (!p || !*p) return 0;
  if (p[0] == '/') return 0;
  if (strstr(p, "..")) return 0;
  if (strchr(p, ':')) return 0;      /* no device prefixes */
  return 1;
}

/* The engine sometimes carries the URI scheme through into the filename.
 * Strip it rather than failing the open: "content:///res_output/x" and
 * "res_output/x" are the same asset, and which one arrives depends on which
 * of the engine's two path builders was used. */
static const char *strip_scheme(const char *p) {
  static const char *const schemes[] = { "content:///", "content://", "file:///", "asset:///" };
  unsigned i;
  for (i = 0; i < sizeof(schemes) / sizeof(*schemes); i++) {
    const size_t n = strlen(schemes[i]);
    if (!strncmp(p, schemes[i], n)) return p + n;
  }
  while (*p == '/') p++;
  return p;
}

void *pps_assetmanager_from_java(void *env, void *java_asset_manager) {
  (void)env; (void)java_asset_manager;
  LOGB("AAssetManager_fromJava -> rooted at %s", pps_assets_root());
  return &g_manager_token;
}

/* mode is AASSET_MODE_UNKNOWN/RANDOM/STREAMING/BUFFER. All four are advisory
 * -- they tell a real implementation how to arrange its backing store -- and
 * the engine passes STREAMING for everything, including files it then reads in
 * one call. So the mode is deliberately ignored and the size decides. */
void *pps_asset_open(void *mgr, const char *filename, int mode) {
  char full[FS_MAX_PATH];
  const char *rel;
  struct stat st;
  Asset *a;
  int n;

  (void)mode;
  if (mgr != &g_manager_token) {
    LOGE("AAssetManager_open with an unknown manager handle");
    return NULL;
  }
  if (!filename) return NULL;

  rel = strip_scheme(filename);
  if (!path_is_safe(rel)) {
    LOGW("AAssetManager_open refused unsafe path: %s", filename);
    return NULL;
  }

  n = snprintf(full, sizeof(full), "%s/%s", pps_assets_root(), rel);
  if (n < 0 || (size_t)n >= sizeof(full)) {
    LOGE("asset path too long: %s", rel);
    return NULL;
  }

  if (stat(full, &st) != 0) {
    /* Not an error by itself. The engine probes for optional assets --
     * per-language overrides, A/B test variants, higher-resolution art -- and
     * expects a null back.
     *
     * RATE LIMITED, and that is not tidiness. log_write flushes to the SD card
     * on every line, which costs milliseconds; the engine probes optional
     * assets in the hundreds during startup, and with DEBUG_LOG on an unlimited
     * version of this line adds real seconds to boot and changes the timing of
     * whatever you were trying to observe. The first few dozen name the file,
     * which is what you need when the asset root is wrong; after that only the
     * count matters, and pps_assets_miss_count() reports it once boot is done. */
    g_misses++;
    if (g_misses <= 32) {
      LOGB("asset miss: %s", rel);
      if (!g_first_miss[0]) snprintf(g_first_miss, sizeof(g_first_miss), "%s", rel);
    } else if (g_misses == 33) {
      LOGB("asset miss: (further misses counted, not listed)");
    }
    return NULL;
  }

  a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->magic = ASSET_MAGIC;
  a->size  = (long)st.st_size;
  a->pos   = 0;

  if (a->size >= 0 && a->size <= ASSET_BUFFER_MAX) {
    FILE *f = fopen_locked(full, "rb");
    if (!f) { free(a); LOGE("asset open failed: %s", rel); return NULL; }
    a->data = malloc((size_t)a->size ? (size_t)a->size : 1);
    if (!a->data) { fclose_locked(f); free(a); return NULL; }
    if (a->size && fread_locked(a->data, 1, (size_t)a->size, f) != (size_t)a->size) {
      LOGE("asset short read: %s", rel);
      fclose_locked(f); free(a->data); free(a);
      return NULL;
    }
    fclose_locked(f);
  } else {
    a->fp = fopen_locked(full, "rb");
    if (!a->fp) { free(a); LOGE("asset open failed: %s", rel); return NULL; }
  }
  return a;
}

/* Returns bytes read, 0 at end of stream, negative on error -- the NDK
 * contract. The engine loops until it gets 0, so returning a short count is
 * fine but returning an error where there is none would truncate a file. */
int pps_asset_read(void *asset, void *buf, size_t count) {
  Asset *a = asset;
  size_t got;

  if (!a || a->magic != ASSET_MAGIC || !buf) return -1;
  if (count == 0) return 0;

  if (a->data) {
    long left = a->size - a->pos;
    if (left <= 0) return 0;
    if ((long)count > left) count = (size_t)left;
    memcpy(buf, a->data + a->pos, count);
    a->pos += (long)count;
    return (int)count;
  }

  got = fread_locked(buf, 1, count, a->fp);
  a->pos += (long)got;
  return (int)got;
}

/* AAsset_getLength returns off_t. The engine sizes its allocations from this
 * before reading, so a wrong answer here is a heap overflow rather than a
 * failed load -- which is why the size comes from stat() at open time and is
 * never recomputed. */
long pps_asset_get_length(void *asset) {
  Asset *a = asset;
  if (!a || a->magic != ASSET_MAGIC) return 0;
  return a->size;
}

void pps_asset_close(void *asset) {
  Asset *a = asset;
  if (!a || a->magic != ASSET_MAGIC) return;
  a->magic = 0;
  if (a->fp) fclose_locked(a->fp);
  free(a->data);
  free(a);
}

/* ------------------------------------------------------------------ */
/* AConfiguration                                                      */
/* ------------------------------------------------------------------ */

/* The engine builds one of these to ask a single question --
 * AConfiguration_getScreenSize -- and then deletes it. It uses the answer to
 * pick which tier of art to load.
 *
 * ACONFIGURATION_SCREENSIZE_LARGE is 3 and XLARGE is 4. LARGE is the honest
 * answer for a 1920x1080 surface at the density we report: XLARGE is the
 * 10-inch tablet bucket, and asking for that tier would have the engine look
 * for art the phone/tablet APK does not ship. A missing tier does not fall
 * back gracefully here -- the asset simply misses and the sprite is absent. */
#define ACONFIGURATION_SCREENSIZE_LARGE 3

static int g_config_token;

void *pps_config_new(void) { return &g_config_token; }
void  pps_config_delete(void *cfg) { (void)cfg; }

void pps_config_from_asset_manager(void *cfg, void *mgr) {
  (void)cfg; (void)mgr;
}

int pps_config_get_screen_size(void *cfg) {
  (void)cfg;
  return ACONFIGURATION_SCREENSIZE_LARGE;
}

/* Reported by main.c after bring-up. A game that renders nothing but opened
 * everything is a different problem from one that could not find its files,
 * and without this the two look identical from the outside. */
int pps_assets_miss_count(void) { return g_misses; }
const char *pps_assets_first_miss(void) { return g_first_miss; }
