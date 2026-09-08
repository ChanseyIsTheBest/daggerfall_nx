/* pps_music.c -- the game's music, decoded and mixed here.
 *
 * MIT licensed. See LICENSE.
 *
 * WHY MUSIC IS NOT THE SAME PATH AS SOUND EFFECTS
 * -----------------------------------------------
 * This engine splits its audio in two, and the split is visible in both the
 * binary and the assets. Getting it wrong means either silence or two audio
 * devices fighting, so it is worth writing down.
 *
 *   Sound effects -- assets/res_output/sound/sounds/, 152 files, mono 22050 Hz
 *       ff::Audio::SoundPlayerAndroid reads the .ogg through the asset
 *       manager, decodes it with the stb_vorbis STATICALLY LINKED INSIDE
 *       libpapapearsaga.so (the string "stb_vorbis_open_memory failed with
 *       error" is in its .rodata), and feeds the PCM to an OpenSL buffer
 *       queue. That is entirely the engine's business; opensles.c handles it
 *       and this file is not involved.
 *
 *   Music -- assets/res_output/sound/music/, 17 files, mono 44100 Hz
 *       ff::Audio::MusicPlayerAndroid does NOT decode. It reads the .ogg into
 *       a byte[] and hands the compressed bytes to Java:
 *
 *           com/king/core/MusicManager.LoadResource(String, byte[])I
 *
 *       On Android the Java side fed those bytes to MediaPlayer, which decoded
 *       and played them. There is no MediaPlayer here, so this file is where
 *       that decode has to happen.
 *
 * The class path "com/king/core/MusicManager" and the signature
 * "(Ljava/lang/String;[B)I" are both present in the library, which is what
 * confirms the second path is live rather than vestigial.
 *
 * SIZING
 * ------
 * The music files are short loops: 21-25 seconds, mono, 44100 Hz. Decoded to
 * S16 that is about 2 MB each, and the engine holds one at a time because the
 * Java MusicManager owned a single MediaPlayer. So each resource is decoded
 * once, in full, at load time -- no streaming, no decode on the audio thread,
 * and no allocation inside the callback.
 *
 * OUTPUT
 * ------
 * Through opensles.c's music hook, NOT a second SDL device. opensles.c already
 * owns the one output; a second device would either fail to open or fight this
 * one for the mixer. The Sonic Jump port shipped that bug once and its
 * whole-tree link check is what found it.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "pps_music.h"
#include "pps_jni.h"
#include "opensles.h"

/* stb_vorbis is compiled INTO this file rather than built separately, which is
 * how it is meant to be used and keeps its ~5000 lines out of the link line
 * for everything else.
 *
 * NO_PUSHDATA_API drops the incremental parser -- these files are decoded in
 * one call from a complete buffer. NO_INTEGER_CONVERSION must NOT be defined:
 * it removes stb_vorbis_decode_memory, which is the one function this file
 * actually calls. (It reads as an optimisation and is a decapitation.)
 *
 * NO_STDIO drops the fopen-based entry points; the bytes always arrive from
 * the engine as a byte[], never as a path.
 *
 * The file is named .inc rather than .c ON PURPOSE. The Makefile builds
 * source/[star].c by globbing, so a .c here would be compiled standalone AS WELL AS
 * included, giving two definitions of all ~40 stb_vorbis entry points. That is
 * a clean compile and a failed link; tools/check_links.py caught it. */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO

/* Warnings suppressed for the vendored decoder only, and re-enabled straight
 * after so nothing in this port hides behind them.
 *
 * stb_vorbis's get_seek_page_info fills `header` through getn() and then reads
 * it; GCC cannot see through the call and reports eight maybe-uninitialized
 * warnings. They are upstream, they are false, and a wall of them in the build
 * output is how a real warning gets skimmed past. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "stb_vorbis.inc"
#pragma GCC diagnostic pop

/* The engine loads one track at a time, but it releases the old one after
 * loading the new one on some transitions, so a couple of slots are needed.
 * Four is comfortable and costs nothing when empty. */
#define MAX_TRACKS 4

typedef struct {
  int      used;
  int16_t *pcm;        /* interleaved, always stereo after load */
  int      frames;     /* per channel */
  int      rate;
  char     name[64];
} Track;

static Track  g_track[MAX_TRACKS];
static Mutex  g_lock;
static int    g_lock_ready;

/* Playback state. Read and written from both the game thread and the audio
 * callback, so everything here is under g_lock. */
static int    g_cur = -1;       /* index into g_track, or -1              */
static int    g_playing;
static int    g_suspended;
static int    g_enabled = 1;
static int    g_loops_left;     /* -1 = forever                           */
static int    g_loops_done;
static double g_pos;            /* fractional source frame                */
static float  g_volume = 1.0f;

static void lock_init(void) {
  if (!g_lock_ready) { mutexInit(&g_lock); g_lock_ready = 1; }
}

/* ------------------------------------------------------------------ */
/* the mix callback                                                    */
/* ------------------------------------------------------------------ */

/* Called from the SDL audio thread through opensles.c. Must not block and
 * must not allocate -- which is why the decode happens at load time.
 *
 * Resampling is linear from the file's rate to the device's. The sources are
 * 44100 and the device is usually 48000, so a 1.088 ratio: linear
 * interpolation is inaudible on that, and anything better would cost more
 * than it is worth for a background loop. */
static int music_mix(void *ctx, int16_t *dst, int frames) {
  int written = 0;
  Track *t;
  double step;
  const int out_rate = opensles_output_rate();

  (void)ctx;

  mutexLock(&g_lock);

  if (!g_playing || g_suspended || !g_enabled || g_cur < 0) {
    mutexUnlock(&g_lock);
    return 0;
  }

  t = &g_track[g_cur];
  if (!t->used || !t->pcm || t->frames <= 0) {
    mutexUnlock(&g_lock);
    return 0;
  }

  step = (double)t->rate / (double)(out_rate > 0 ? out_rate : 48000);

  while (written < frames) {
    int i0, i1;
    double frac;
    int32_t l, r;

    if (g_pos >= (double)t->frames) {
      /* End of the loop. loops_left < 0 is "forever", which is what the
       * engine asks for on the map and in-game themes. */
      if (g_loops_left < 0) {
        g_pos -= (double)t->frames;
        g_loops_done++;
      } else if (g_loops_left > 0) {
        g_loops_left--;
        g_loops_done++;
        g_pos -= (double)t->frames;
      } else {
        g_playing = 0;
        break;
      }
    }

    i0 = (int)g_pos;
    i1 = i0 + 1;
    if (i1 >= t->frames) i1 = (g_loops_left != 0) ? 0 : i0;
    frac = g_pos - (double)i0;

    l = (int32_t)((1.0 - frac) * t->pcm[i0 * 2 + 0] + frac * t->pcm[i1 * 2 + 0]);
    r = (int32_t)((1.0 - frac) * t->pcm[i0 * 2 + 1] + frac * t->pcm[i1 * 2 + 1]);

    l = (int32_t)(l * g_volume);
    r = (int32_t)(r * g_volume);
    if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
    if (r >  32767) r =  32767; else if (r < -32768) r = -32768;

    dst[written * 2 + 0] = (int16_t)l;
    dst[written * 2 + 1] = (int16_t)r;
    written++;
    g_pos += step;
  }

  mutexUnlock(&g_lock);
  return written;
}

void pps_music_init(void) {
  lock_init();
  opensles_set_music_source(music_mix, NULL);
  LOGB("music: registered with the OpenSL output");
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static void free_track(Track *t) {
  if (t->pcm) free(t->pcm);
  t->pcm = NULL;
  t->frames = 0;
  t->used = 0;
}

/* Decode the whole file. Returns a 1-based handle, or 0 on failure.
 *
 * Returning 0 matters: the engine treats a zero handle as a failed load and
 * moves on without retrying, which is the behaviour we want if a file is
 * corrupt. A non-zero handle for a track that will never play would leave it
 * believing music is running. */
int pps_music_load(const char *name, const void *ogg, int ogg_len) {
  int slot = -1, i, ch = 0, rate = 0, n;
  short *decoded = NULL;
  int16_t *stereo;

  lock_init();

  if (!ogg || ogg_len <= 0) return 0;

  /* stb_vorbis_decode_memory allocates the output with malloc and returns the
   * number of frames PER CHANNEL, with samples interleaved. It is the same
   * decoder the engine itself carries for sound effects, so the two paths
   * agree on what these files contain. */
  n = stb_vorbis_decode_memory((const unsigned char *)ogg, ogg_len,
                               &ch, &rate, &decoded);
  if (n <= 0 || !decoded || ch <= 0) {
    LOGW("music: could not decode %s (%d bytes)", name ? name : "?", ogg_len);
    if (decoded) free(decoded);
    return 0;
  }

  mutexLock(&g_lock);
  for (i = 0; i < MAX_TRACKS; i++) if (!g_track[i].used) { slot = i; break; }
  if (slot < 0) {
    /* Every slot taken. Reuse the one that is not playing rather than failing:
     * the engine does not always release before loading. */
    for (i = 0; i < MAX_TRACKS; i++) if (i != g_cur) { slot = i; break; }
    if (slot < 0) slot = 0;
    free_track(&g_track[slot]);
    LOGW("music: no free slot; reused %d", slot);
  }

  /* Store as stereo regardless. These files are all mono, and duplicating the
   * channel once at load is cheaper and simpler than branching per sample in
   * the callback. 2 MB per track either way. */
  stereo = (int16_t *)malloc((size_t)n * 2 * sizeof(int16_t));
  if (!stereo) {
    mutexUnlock(&g_lock);
    free(decoded);
    LOGE("music: out of memory decoding %s (%d frames)", name ? name : "?", n);
    return 0;
  }

  if (ch == 1) {
    for (i = 0; i < n; i++) {
      stereo[i * 2 + 0] = decoded[i];
      stereo[i * 2 + 1] = decoded[i];
    }
  } else {
    for (i = 0; i < n; i++) {
      stereo[i * 2 + 0] = decoded[i * ch + 0];
      stereo[i * 2 + 1] = decoded[i * ch + 1];
    }
  }
  free(decoded);

  g_track[slot].used = 1;
  g_track[slot].pcm = stereo;
  g_track[slot].frames = n;
  g_track[slot].rate = rate;
  snprintf(g_track[slot].name, sizeof(g_track[slot].name), "%s", name ? name : "?");
  mutexUnlock(&g_lock);

  LOGB("music: loaded %s -- %d frames, %d Hz, %dch -> handle %d (%.1f s, %.1f KB)",
       g_track[slot].name, n, rate, ch, slot + 1,
       (double)n / (rate ? rate : 44100), (double)n * 4.0 / 1024.0);
  return slot + 1;
}

void pps_music_release(int handle) {
  const int i = handle - 1;
  if (i < 0 || i >= MAX_TRACKS) return;
  lock_init();
  mutexLock(&g_lock);
  if (g_cur == i) { g_playing = 0; g_cur = -1; }
  free_track(&g_track[i]);
  mutexUnlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* transport                                                           */
/* ------------------------------------------------------------------ */

/* Play(int handle, int loopCount, float volume).
 *
 * The loop count convention is the engine's: 0 plays once, a negative value
 * loops forever, and n > 0 plays n+1 times. That is what MediaPlayer's
 * setLooping plus the Java wrapper's own counter produced. */
void pps_music_play(int handle, int loops, float volume) {
  const int i = handle - 1;
  lock_init();
  mutexLock(&g_lock);
  if (i < 0 || i >= MAX_TRACKS || !g_track[i].used) {
    mutexUnlock(&g_lock);
    LOGW("music: Play on an unknown handle %d", handle);
    return;
  }
  g_cur = i;
  g_pos = 0.0;
  g_loops_left = loops;
  g_loops_done = 0;
  g_volume = volume * cfg_master_volume();
  if (g_volume < 0.0f) g_volume = 0.0f;
  if (g_volume > 1.0f) g_volume = 1.0f;
  g_playing = 1;
  g_suspended = 0;
  mutexUnlock(&g_lock);
  LOGB("music: play %s loops=%d vol=%.2f", g_track[i].name, loops, (double)g_volume);
}

void pps_music_stop(void) {
  lock_init();
  mutexLock(&g_lock);
  g_playing = 0;
  g_pos = 0.0;
  mutexUnlock(&g_lock);
}

void pps_music_suspend(void) { lock_init(); mutexLock(&g_lock); g_suspended = 1; mutexUnlock(&g_lock); }
void pps_music_resume(void)  { lock_init(); mutexLock(&g_lock); g_suspended = 0; mutexUnlock(&g_lock); }

void pps_music_set_volume(float v) {
  lock_init();
  mutexLock(&g_lock);
  g_volume = v * cfg_master_volume();
  if (g_volume < 0.0f) g_volume = 0.0f;
  if (g_volume > 1.0f) g_volume = 1.0f;
  mutexUnlock(&g_lock);
}

void pps_music_set_enabled(int on) {
  lock_init();
  mutexLock(&g_lock);
  g_enabled = on ? 1 : 0;
  mutexUnlock(&g_lock);
}

int   pps_music_is_enabled(void) { return g_enabled; }
int   pps_music_is_playing(void) { return g_playing && !g_suspended; }
int   pps_music_loop_count(void) { return g_loops_done; }

float pps_music_length(void) {
  float r = 0.0f;
  lock_init();
  mutexLock(&g_lock);
  if (g_cur >= 0 && g_track[g_cur].used && g_track[g_cur].rate > 0)
    r = (float)g_track[g_cur].frames / (float)g_track[g_cur].rate;
  mutexUnlock(&g_lock);
  return r;
}

float pps_music_position(void) {
  float r = 0.0f;
  lock_init();
  mutexLock(&g_lock);
  if (g_cur >= 0 && g_track[g_cur].used && g_track[g_cur].rate > 0)
    r = (float)(g_pos / (double)g_track[g_cur].rate);
  mutexUnlock(&g_lock);
  return r;
}

void pps_music_shutdown(void) {
  int i;
  lock_init();
  mutexLock(&g_lock);
  g_playing = 0;
  g_cur = -1;
  for (i = 0; i < MAX_TRACKS; i++) free_track(&g_track[i]);
  mutexUnlock(&g_lock);
  opensles_set_music_source(NULL, NULL);
}
