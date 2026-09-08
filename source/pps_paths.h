/* pps_paths.h -- where the game's files live. MIT licensed.
 *
 * Unlike the Osmos port, this engine does NOT read assets as plain files from
 * a data directory. It goes through the NDK asset manager: libpapapearsaga.so
 * imports AAssetManager_fromJava, AAssetManager_open, AAsset_read,
 * AAsset_getLength and AAsset_close, and every asset path it builds is
 * relative to a "content:///res_output/" URI root.
 *
 * So there are two distinct roots here and they are not the same directory:
 *
 *   pps_root()      the folder holding the .nro, libpapapearsaga.so, config.txt,
 *                   debug.log, purchases.txt and the save data
 *   pps_assets_root()  what AAssetManager is rooted at -- <root>/assets, so
 *                   that the engine's "res_output/scenes/root.xml" resolves to
 *                   <root>/assets/res_output/scenes/root.xml
 *
 * And a third the engine asks for by name through FileSystem.getHomeDirectory:
 *
 *   pps_home_dir()  writable storage, <root>/storage/, WITH a trailing slash
 *                   because the Java implementation returned one and the
 *                   engine concatenates without adding its own.
 */
#ifndef PPS_PATHS_H
#define PPS_PATHS_H

/* Find the game. The .nro may live in any folder under /switch, and the game
 * data may sit beside it or in a folder of its own. Must run before cfg_load()
 * and log_init(), both of which live in the directory this finds. */
void        pps_paths_init(void);
void        pps_paths_log_search(void);  /* replay the search into debug.log */
int         pps_paths_check(void);       /* 1 if the library and assets are there */
const char *pps_paths_error(void);       /* why check() failed */

const char *pps_root(void);          /* the folder the .nro was launched from  */
const char *pps_so_path(void);       /* <root>/libpapapearsaga.so              */
const char *pps_assets_root(void);   /* <root>/assets -- the AAssetManager root */

/* The three directories FileSystem hands the engine. ALL END WITH '/'. That is
 * not cosmetic: the Java versions appended one explicitly and the engine
 * concatenates a bare filename onto the result, so dropping it silently turns
 * "<dir>/save.dat" into "<dir>save.dat" one level up. */
const char *pps_home_dir(void);      /* getHomeDirectory   -- persistent saves */
const char *pps_cache_dir(void);     /* getCacheDirectory  -- scratch          */
const char *pps_shared_dir(void);    /* getSharedDirectory -- external storage */

const char *pps_device_uuid(void);   /* stable per-console id, generated once  */

/* Create the writable directories if they are not there yet. The engine
 * assumes its home directory exists and does not mkdir it. */
void        pps_paths_make_dirs(void);

#endif
