/* pps_assets.h -- the NDK asset manager, backed by <gamedir>/assets.
 * MIT licensed. See pps_assets.c.
 */
#ifndef PPS_ASSETS_H
#define PPS_ASSETS_H

#include <stddef.h>
#include <stdint.h>

void *pps_assetmanager_from_java(void *env, void *java_asset_manager);
void *pps_asset_open(void *mgr, const char *filename, int mode);
int   pps_asset_read(void *asset, void *buf, size_t count);
long  pps_asset_get_length(void *asset);
void  pps_asset_close(void *asset);

void *pps_config_new(void);
void  pps_config_delete(void *cfg);
void  pps_config_from_asset_manager(void *cfg, void *mgr);
int   pps_config_get_screen_size(void *cfg);

/* How many opens found nothing, and the first of them. A handful is normal;
 * thousands means the asset root is wrong. main.c reports this after boot. */
int   pps_assets_miss_count(void);
const char *pps_assets_first_miss(void);

#endif
