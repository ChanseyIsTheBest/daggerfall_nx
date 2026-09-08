/* pps_platform.h -- the com.king.* platform classes. MIT licensed.
 * See pps_platform.c for what is implemented and what is deliberately inert.
 */
#ifndef PPS_PLATFORM_H
#define PPS_PLATFORM_H

#include "pps_jni.h"

/* What the engine asked for through GameLib.setupPlatform. Read back by
 * main.c so the frame pacing matches what the engine believes. */
typedef struct {
  int width, height, rotation, target_fps, msaa;
} PpsPlatformSetup;

const PpsPlatformSetup *pps_platform_setup(void);
int  pps_platform_keyboard_showing(void);

/* Sub-dispatchers, split out so this file stays about the platform layer.
 * Both return 1 if the call was handled. */
int pps_music_call(const char *name, const char *sig,
                   const PpsArgs *a, PpsArgs *out);
int pps_purchases_call(const char *cls, const char *name, const char *sig,
                       void *recv, const PpsArgs *a, PpsArgs *out);

#endif
