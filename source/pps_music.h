/* pps_music.h -- the game's music. MIT licensed. See pps_music.c.
 *
 * Music and sound effects take DIFFERENT paths in this engine, and only music
 * reaches this file:
 *
 *   SFX   the engine decodes its own .ogg with a statically linked stb_vorbis
 *         and feeds OpenSL directly. Nothing here is involved.
 *   Music the engine hands the COMPRESSED bytes to Java --
 *         MusicManager.LoadResource(String, byte[])I -- because on Android
 *         MediaPlayer decoded them. There is no MediaPlayer here, so this is
 *         where that decode happens.
 */
#ifndef PPS_MUSIC_H
#define PPS_MUSIC_H

/* Register with opensles.c's music hook. Call once, before the engine starts.
 * Deliberately not a second SDL audio device -- opensles.c owns the only one. */
void  pps_music_init(void);
void  pps_music_shutdown(void);

/* Decode a whole Ogg Vorbis file to PCM. Returns a 1-based handle, or 0 if the
 * data would not decode -- the engine reads 0 as a failed load and moves on,
 * which is the right outcome for a corrupt file. */
int   pps_music_load(const char *name, const void *ogg, int ogg_len);
void  pps_music_release(int handle);

/* loops: 0 plays once, negative loops forever, n > 0 plays n+1 times. */
void  pps_music_play(int handle, int loops, float volume);
void  pps_music_stop(void);
void  pps_music_suspend(void);
void  pps_music_resume(void);
void  pps_music_set_volume(float v);
void  pps_music_set_enabled(int on);

int   pps_music_is_enabled(void);
int   pps_music_is_playing(void);
int   pps_music_loop_count(void);
float pps_music_length(void);     /* seconds */
float pps_music_position(void);   /* seconds */

#endif
