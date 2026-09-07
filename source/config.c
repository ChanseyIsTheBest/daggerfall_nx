/* config.c -- PvZ Fusion 3.8.1 Switch port
 *
 * There is no runtime configuration any more, so there is no parser here and no
 * config.txt is read or written. What used to be configurable:
 *
 *   portrait                      retired -- the port is landscape only, and the
 *                                 pointer layer (nx_pointer) assumes an
 *                                 unrotated panel. See PORTING.md section 5.
 *   language                      retired -- the game's Simplified Chinese path
 *                                 does not work in this port, so there is
 *                                 nothing to choose. lang_code() in jni_fake.c
 *                                 always reports English.
 *   screen_width / screen_height  retired -- these were parsed and written but
 *                                 never actually applied to anything. The real
 *                                 render size is the panel size, set below and
 *                                 then owned by android_native_update_mode(),
 *                                 which re-derives it every frame from the
 *                                 docked/handheld state.
 *
 * The build-time knobs (memory layout, paths, DEBUG_LOG, ...) live in config.h
 * and are compiled in deliberately: they are engine-fitting parameters, not user
 * settings, and a wrong value there does not misbehave, it fails to boot.
 *
 * This file still exists, rather than being deleted, because assemble.sh copies
 * the Zookeeper core's config.c into source/ before the overlay is applied --
 * so this overlay copy has to be here to win, and it is the definition site for
 * the two screen globals that the rest of the port links against.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "config.h"

/* Actual render size in use right now. PvZ Fusion is LANDSCAPE, so this starts
 * at the handheld panel size; android_native_update_mode() replaces it with
 * 1920x1080 when docked and 1280x720 when handheld, every frame. Read by
 * jni_fake.c (DisplayMetrics), unity_jni.c (Display getters) and imports.c
 * (the EGL/GL surface-size substitutions). */
int screen_width  = 1280;
int screen_height = 720;
