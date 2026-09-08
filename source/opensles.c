/* Minimal OpenSL ES implementation backed by SDL2. */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "opensles.h"
#include "util.h"
#include "mp3_decode.h"
#include <unistd.h>
#include <fcntl.h>
/* Happy Wheels mixed its Cocos video-player audio in here. Sonic Jump has no
 * video playback -- no MediaPlayer video path, no .mp4 in the asset set -- so
 * the hook is gone and the mixer emits SFX plus music only. */

/* OpenSL ES constants. */

#define SL_RESULT_SUCCESS              0x00
#define SL_RESULT_PARAMETER_INVALID    0x02
#define SL_RESULT_MEMORY_FAILURE       0x03
#define SL_RESULT_RESOURCE_ERROR       0x04
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C
#define SL_RESULT_INTERNAL_ERROR       0x0D
#define SL_RESULT_CONTENT_UNSUPPORTED  0x09

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef int16_t  SLint16;
typedef uint8_t  SLuint8;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t  SLmillibel;

// PCM data format (samplesPerSec is in milliHz per the spec)
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void *SLObjectItf;       // -> &obj->obj_vt
typedef void *SLInterfaceID;

typedef void (*slBufferQueueCallback)(void *caller, void *context);

/* Interface IDs. */

#define DEF_IID(n) void *SL_IID_##n = &SL_IID_##n
DEF_IID(3DCOMMIT); DEF_IID(3DDOPPLER); DEF_IID(3DGROUPING); DEF_IID(3DLOCATION);
DEF_IID(3DMACROSCOPIC); DEF_IID(3DSOURCE); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(ANDROIDEFFECT); DEF_IID(ANDROIDEFFECTCAPABILITIES); DEF_IID(ANDROIDEFFECTSEND);
DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(AUDIODECODERCAPABILITIES); DEF_IID(AUDIOENCODER);
DEF_IID(AUDIOENCODERCAPABILITIES); DEF_IID(AUDIOIODEVICECAPABILITIES); DEF_IID(BASSBOOST);
DEF_IID(BUFFERQUEUE); DEF_IID(DEVICEVOLUME); DEF_IID(DYNAMICINTERFACEMANAGEMENT);
DEF_IID(DYNAMICSOURCE); DEF_IID(EFFECTSEND); DEF_IID(ENGINE); DEF_IID(ENGINECAPABILITIES);
DEF_IID(ENVIRONMENTALREVERB); DEF_IID(EQUALIZER); DEF_IID(LED); DEF_IID(METADATAEXTRACTION);
DEF_IID(METADATATRAVERSAL); DEF_IID(MIDIMESSAGE); DEF_IID(MIDIMUTESOLO); DEF_IID(MIDITEMPO);
DEF_IID(MIDITIME); DEF_IID(MUTESOLO); DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(OUTPUTMIX);
DEF_IID(PITCH); DEF_IID(PLAY); DEF_IID(PLAYBACKRATE); DEF_IID(PREFETCHSTATUS);
DEF_IID(PRESETREVERB); DEF_IID(RATEPITCH); DEF_IID(RECORD); DEF_IID(SEEK); DEF_IID(THREADSYNC);
DEF_IID(VIBRA); DEF_IID(VIRTUALIZER); DEF_IID(VISUALIZATION); DEF_IID(VOLUME);
#undef DEF_IID

/* OpenSL ES 1.0.1 vtables. */

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *pPriority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateAudioRecorder;
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *pMix, SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateMetadataExtractor;
  void *CreateExtensionObject;
  void *QueryNumSupportedInterfaces;
  void *QuerySupportedInterfaces;
  void *QueryNumSupportedExtensions;
  void *QuerySupportedExtension;
  void *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} SLPlayItf_;

typedef void (*slPlayCallback)(void *caller, void *pContext, SLuint32 event);

#define SL_PLAYEVENT_HEADATEND 0x00000001u

typedef void (*slPrefetchCallback)(void *caller, void *pContext, SLuint32 event);

#define SL_PREFETCHSTATUS_SUFFICIENTDATA 3
#define SL_PREFETCHEVENT_STATUSCHANGE    0x01u
#define SL_PREFETCHEVENT_FILLLEVELCHANGE 0x02u

typedef struct {
  SLresult (*GetPrefetchStatus)(void *self, SLuint32 *pStatus);
  SLresult (*GetFillLevel)(void *self, SLuint32 *pLevel);
  SLresult (*RegisterCallback)(void *self, slPrefetchCallback cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetFillUpdatePeriod)(void *self, SLuint32 period);
  SLresult (*GetFillUpdatePeriod)(void *self, SLuint32 *pPeriod);
} SLPrefetchStatusItf_;

typedef struct {
  SLresult (*SetPosition)(void *self, SLuint32 pos, SLuint32 seekMode);
  SLresult (*SetLoop)(void *self, SLboolean enable, SLuint32 startPos, SLuint32 endPos);
  SLresult (*GetLoop)(void *self, SLboolean *pEnable, SLuint32 *pStart, SLuint32 *pEnd);
} SLSeekItf_;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *pState);
  SLresult (*RegisterCallback)(void *self, slBufferQueueCallback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *p);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *p);
  SLresult (*SetStereoPosition)(void *self, SLint32 perMille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *p);
} SLVolumeItf_;

typedef struct {
  SLresult (*SetRate)(void *self, SLint16 rate);
  SLresult (*GetRate)(void *self, SLint16 *p);
  SLresult (*SetPropertyConstraints)(void *self, SLuint32 c);
  SLresult (*GetProperties)(void *self, SLuint32 *p);
  SLresult (*GetCapabilitiesOfRate)(void *self, SLuint32 *p);
  SLresult (*GetRateRange)(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop);
} SLPlaybackRateItf_;

typedef struct {
  SLresult (*SetConfiguration)(void *self, const void *key, const void *value, SLuint32 valueSize);
  SLresult (*GetConfiguration)(void *self, const void *key, SLuint32 *pValueSize, void *value);
  SLresult (*AcquireJavaProxy)(void *self, SLuint32 proxyType, void *pProxyObj);
  SLresult (*ReleaseJavaProxy)(void *self, SLuint32 proxyType);
} SLAndroidConfigurationItf_;

/* Objects. */

#define MAX_PLAYERS 64
#define BQ_SLOTS 256

typedef struct {
  const void *data;
  SLuint32 size;
} BQBuffer;

typedef struct Player {
  const SLObjectItf_ *obj_vt;
  const SLPlayItf_   *play_vt;
  const SLBufferQueueItf_ *bq_vt;
  const SLVolumeItf_ *vol_vt;
  const SLPlaybackRateItf_ *rate_vt;
  const SLAndroidConfigurationItf_ *config_vt;
  const SLSeekItf_   *seek_vt;
  const SLPrefetchStatusItf_ *prefetch_vt;

  int in_use;
  int channels;
  int rate;
  int sbytes;    // bytes per sample in the enqueued buffers (2=16-bit, 4=32-bit)
  int is_float;  // 1 if samples are 32-bit float, 0 if signed integer
  int playing;
  int drained;   // consecutive callbacks this playing player produced no audio
  float gain; // linear, from SetVolumeLevel (millibels)

  slBufferQueueCallback cb;
  void *cb_ctx;

  // FIFO of enqueued buffers
  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail; // count = (tail - head + N) % N
  // currently draining buffer
  const uint8_t *cur;
  SLuint32 cur_size;
  double cur_fpos; // fractional sample index into cur (for rate conversion)
  /* Playback rate in per-mille (1000 == normal), set through SL_IID_PLAYBACKRATE.
   * Sonic Jump imports that interface, so it does vary SFX pitch -- games use it
   * so a sound triggered repeatedly (rings, jumps) does not sound identical
   * every time. Leaving SetRate a no-op is audible: every effect plays at a
   * fixed pitch. Read without a lock; a torn read is impossible for a 16-bit
   * aligned store and the worst case is one block at the previous rate. */
  SLint16 play_rate;

  /* Decoded-file source (SL_DATALOCATOR_ANDROIDFD). Instead of waiting to be
   * fed by the engine, the player owns its whole PCM buffer and re-arms itself
   * from the start each pass. */
  int       is_decoded;
  int16_t  *own_pcm;
  SLuint32  own_bytes;
  size_t    total_frames;
  int       loop;
  int       finished;
  SLuint32  state;      /* last SL_PLAYSTATE_* requested, reported verbatim */

  /* SLPlayItf callback, used to report SL_PLAYEVENT_HEADATEND. */
  slPlayCallback play_cb;
  void          *play_cb_ctx;
  SLuint32       play_events;

  slPrefetchCallback prefetch_cb;
  void              *prefetch_cb_ctx;
  SLuint32           prefetch_events;
  int                prefetch_announced;

  SDL_mutex *lock;
} Player;

typedef struct {
  const SLObjectItf_ *obj_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *obj_vt;
  const SLEngineItf_ *eng_vt;
} Engine;

#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

/* SDL audio state. */

static int g_decoded_live;
/* Audio diagnostics. "No sound" has several very different causes and they
 * are indistinguishable from outside: the device may not be open, the callback
 * may not be firing, the mixer may be running but summing silence because no
 * voice is playing, or it may be producing samples that never reach the
 * speaker. These counters separate those cases in one heartbeat line. */
unsigned sj_n_audio_cb;      /* SDL callback invocations */
unsigned sj_n_enqueued;      /* buffers handed to buffer-queue players */
unsigned sj_audio_peak;      /* peak |sample| in the most recent buffer */
unsigned sj_n_voices_live;   /* voices that contributed to the last buffer */
unsigned sj_n_music_frames;  /* frames pulled from the music source */

static SDL_AudioDeviceID g_dev = 0;
static int g_dev_rate = 44100;
static Player *g_players[MAX_PLAYERS];
static int g_player_count = 0;
static SDL_mutex *g_reg_lock = NULL;

static float mb_to_linear(SLmillibel mb) {
  if (mb <= -9600) return 0.0f;
  return powf(10.0f, (float)mb / 2000.0f); // 100 mB = 1 dB
}

// Convert supported PCM formats to signed 16-bit range.
static inline int32_t read_sample_s16(const void *buf, long k, int sbytes, int is_float) {
  if (is_float) {
    float f = ((const float *)buf)[k];
    if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
    return (int32_t)(f * 32767.0f);
  }
  if (sbytes == 4)
    return ((const int32_t *)buf)[k] >> 16;   // S32 -> S16 range
  return (int32_t)((const int16_t *)buf)[k];  // S16
}

/* Mix one player. Returns 1 if a decoded source just reached its end, so the
 * caller can fire SL_PLAYEVENT_HEADATEND after dropping the registry lock --
 * cocos destroys the player from that callback, and doing it mid-iteration
 * would tear the player down underneath this loop. */
static int mix_player(Player *p, int32_t *acc, int frames) {
  int hit_end = 0;
  if (!p->playing)
    return 0;

  // Track finished one-shot players for recycling.
  SDL_LockMutex(p->lock);
  const int dry = p->is_decoded ? p->finished
                                : ((!p->cur) && (p->q_head == p->q_tail));
  SDL_UnlockMutex(p->lock);
  if (dry) { if (p->drained < (1 << 20)) p->drained++; return 0; }
  p->drained = 0;

  const float g = p->gain;
  const int stereo = (p->channels >= 2);
  const int sbytes = p->sbytes > 0 ? p->sbytes : 2;    // bytes per sample
  const int is_float = p->is_float;
  const int bps = stereo ? sbytes * 2 : sbytes;        // bytes per input frame
  // Resample each player to the device rate, scaled by its playback rate.
  const int rate_pm = p->play_rate ? p->play_rate : 1000;
  const double ratio = (g_dev_rate > 0 ? (double)p->rate / (double)g_dev_rate : 1.0)
                     * ((double)rate_pm / 1000.0);

  for (int i = 0; i < frames; i++) {
    // Carry fractional sample positions across buffers.
    for (;;) {
      if (!p->cur) {
        if (p->is_decoded) {
          if (p->finished)
            return hit_end;         // played out; rest of the block is silent
          p->cur      = (const uint8_t *)p->own_pcm;
          p->cur_size = p->own_bytes;
        } else {
          SDL_LockMutex(p->lock);
          const int have = (p->q_head != p->q_tail);
          BQBuffer b = { NULL, 0 };
          if (have) {
            b = p->q[p->q_head];
            p->q_head = (p->q_head + 1) % BQ_SLOTS;
          }
          SDL_UnlockMutex(p->lock);
          if (!have)
            return hit_end; // underrun: rest of the block stays silent
          p->cur = b.data;
          p->cur_size = b.size;
        }
      }
      const long n = (long)(p->cur_size / (SLuint32)bps);
      if (n > 0 && (long)p->cur_fpos < n)
        break; // position is inside the current buffer
      // buffer consumed (or empty): carry remainder, notify engine, fetch next
      p->cur_fpos -= (double)n;
      if (p->cur_fpos < 0.0) p->cur_fpos = 0.0;
      p->cur = NULL;
      if (p->is_decoded) {
        /* Looping re-arms from the top on the next pass; otherwise this
         * source is done and the end is reported once, after the lock. */
        if (!p->loop) {
          p->finished = 1;
          p->playing  = 0;
          p->state    = SL_PLAYSTATE_STOPPED;
          hit_end     = 1;
        }
      } else if (p->cb) {
        p->cb(&p->bq_vt, p->cb_ctx);
      }
    }

    const long n = (long)(p->cur_size / (SLuint32)bps);
    /* p->cur_fpos can be moved by seek_SetPosition() from another thread
     * between the bounds test above and this read. Clamp: a momentary glitch
     * on a seek is acceptable, reading outside the buffer is not. */
    long idx = (long)p->cur_fpos;
    double frac = p->cur_fpos - (double)idx;
    if (idx < 0)      { idx = 0;     frac = 0.0; }
    else if (idx >= n){ idx = n - 1; frac = 0.0; }
    const void *s = p->cur;
    int32_t l, r;
    if (stereo) {
      const long j0 = idx * 2, j1 = (idx + 1 < n ? idx + 1 : idx) * 2;
      const int32_t l0 = read_sample_s16(s, j0,     sbytes, is_float);
      const int32_t l1 = read_sample_s16(s, j1,     sbytes, is_float);
      const int32_t r0 = read_sample_s16(s, j0 + 1, sbytes, is_float);
      const int32_t r1 = read_sample_s16(s, j1 + 1, sbytes, is_float);
      l = (int32_t)(l0 * (1.0 - frac) + l1 * frac);
      r = (int32_t)(r0 * (1.0 - frac) + r1 * frac);
    } else {
      const int32_t a  = read_sample_s16(s, idx, sbytes, is_float);
      const int32_t b2 = (idx + 1 < n) ? read_sample_s16(s, idx + 1, sbytes, is_float) : a;
      l = r = (int32_t)(a * (1.0 - frac) + b2 * frac);
    }
    acc[i * 2 + 0] += (int32_t)(l * g);
    acc[i * 2 + 1] += (int32_t)(r * g);
    p->cur_fpos += ratio;
  }
  return hit_end;
}

/* --- music mix hook (sonicjump_nx) ------------------------------------------
 * Sonic Jump's soundtrack is decoded by sj_music.c, but it must not open its
 * own audio device: opensles.c already owns the one SDL output, and a second
 * device would either fail to open or fight this one for the mixer. So the
 * music source is pulled from inside this callback and summed into the same
 * accumulator as the SFX voices, giving one output and one clock.
 * ------------------------------------------------------------------------ */
static sl_music_fn g_music_fn;
static void       *g_music_ctx;

void opensles_set_music_source(sl_music_fn fn, void *ctx) {
  /* Callable before slCreateEngine: sj_music_init registers itself during
   * startup, well before the game creates its engine, and g_reg_lock does not
   * exist yet at that point. SDL_LockMutex(NULL) merely fails, but relying on
   * that is not something to leave in place. */
  if (!g_reg_lock) g_reg_lock = SDL_CreateMutex();
  SDL_LockMutex(g_reg_lock);
  g_music_fn = fn;
  g_music_ctx = ctx;
  SDL_UnlockMutex(g_reg_lock);
}

int opensles_output_rate(void) { return g_dev_rate ? g_dev_rate : 48000; }

static void SDLCALL audio_callback(void *ud, Uint8 *stream, int len) {
  (void)ud;

  // Native callbacks require bionic TLS on the SDL audio thread.
  static uint8_t audio_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  static int tls_ready = 0;
  if (!tls_ready) { install_bionic_tls(audio_tls); tls_ready = 1; }

  const int frames = len / 4; // S16 stereo
  static int32_t acc[8192 * 2];
  if (frames > 8192) { memset(stream, 0, len); return; }
  memset(acc, 0, frames * 2 * sizeof(int32_t));

  /* Snapshot rather than keep the Player pointer: cocos destroys the player
   * from this callback, so reading its fields after the lock is released is a
   * window onto freed memory. */
  struct { slPlayCallback cb; void *ctx; const void *caller; } ended[MAX_PLAYERS];
  int n_ended = 0;

  sj_n_audio_cb++;
  sj_n_voices_live = 0;

  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++) {
    Player *q = g_players[i];
    if (!q || !q->in_use) continue;
    if (q->state == SL_PLAYSTATE_PLAYING) sj_n_voices_live++;
    if (mix_player(q, acc, frames) && n_ended < MAX_PLAYERS &&
        q->play_cb && (q->play_events & SL_PLAYEVENT_HEADATEND)) {
      ended[n_ended].cb     = q->play_cb;
      ended[n_ended].ctx    = q->play_cb_ctx;
      ended[n_ended].caller = &q->play_vt;
      n_ended++;
    }
  }

  /* Music, summed into the same accumulator as the voices above. Held under
   * g_reg_lock so a track change cannot swap the source mid-buffer. */
  if (g_music_fn) {
    static int16_t mus[8192 * 2];
    int got = g_music_fn(g_music_ctx, mus, frames);
    sj_n_music_frames += (unsigned)got;
    for (int i = 0; i < got * 2; i++) acc[i] += mus[i];
  }

  /* Peak of the mixed result. Zero here with the callback firing means the
   * mixer ran and summed nothing -- a source problem, not a device problem. */
  {
    int peak = 0;
    for (int i = 0; i < frames * 2; i++) {
      int a = acc[i] < 0 ? -acc[i] : acc[i];
      if (a > peak) peak = a;
    }
    sj_audio_peak = (unsigned)peak;
  }
  SDL_UnlockMutex(g_reg_lock);

  for (int i = 0; i < n_ended; i++)
    ended[i].cb((void *)ended[i].caller, ended[i].ctx, SL_PLAYEVENT_HEADATEND);

  int16_t *out = (int16_t *)stream;
  for (int i = 0; i < frames * 2; i++) {
    int32_t v = acc[i];
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
  }
}

static void ensure_device(void) {
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (g_dev)
    return;

  /* Both failure paths used to return silently, which made "no sound" and
   * "no audio device" indistinguishable -- the log showed players being
   * created and a mixer running while nothing was ever opened. Say what
   * happened. */
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    debugLogNote("[sl] SDL_InitSubSystem(AUDIO) FAILED: %s\n", SDL_GetError());
    return;
  }

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_callback;

  /* Allow SDL to hand back a different rate or buffer size: the mixer
   * resamples per voice anyway, and refusing a valid device because it chose
   * 44100 would be silly. Format and channel count are NOT negotiable -- the
   * mixer writes interleaved S16 stereo. */
  g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                              SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                              SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if (!g_dev) {
    debugLogNote("[sl] SDL_OpenAudioDevice FAILED: %s\n", SDL_GetError());
    return;
  }
  if (have.format != AUDIO_S16SYS || have.channels != 2) {
    debugLogNote("[sl] device gave format=0x%04x channels=%d, expected S16 "
                 "stereo; closing\n", (unsigned)have.format, have.channels);
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
    return;
  }

  g_dev_rate = have.freq;
  debugLogNote("[sl] audio device open: %d Hz, %d ch, %d-frame buffer "
               "(driver \"%s\")\n",
               have.freq, have.channels, have.samples,
               SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
  SDL_PauseAudioDevice(g_dev, 0);
}

/* Buffer queue. */

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  sj_n_enqueued++;
  SDL_LockMutex(p->lock);
  const int next = (p->q_tail + 1) % BQ_SLOTS;
  if (next == p->q_head) { // full
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PARAMETER_INVALID;
  }
  p->q[p->q_tail].data = pBuffer;
  p->q[p->q_tail].size = size;
  p->q_tail = next;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  p->cur_size = 0;
  p->cur_fpos = 0.0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (pState) {
    SLBufferQueueState *st = pState;
    SDL_LockMutex(p->lock);
    st->count = (p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS + (p->cur ? 1 : 0);
    st->index = 0;
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  p->cb = cb;
  p->cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtable = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

/* Playback. */

static void pf_announce_ready(Player *p);

/* STOPPED, PAUSED and PLAYING are three different things, and collapsing them
 * to a single "playing" flag got all three of them wrong for a decoded source:
 *
 *   - Stopping is defined to rewind; pausing is defined not to. Both simply
 *     cleared the flag, so a stop followed by a play resumed from the middle
 *     of the track instead of restarting it.
 *   - Replaying a source that had already played out did nothing at all. The
 *     mixer skips a finished decoded source, so it stayed silent for ever
 *     while cocos believed it was playing.
 *   - GetPlayState reported a paused player as STOPPED, which is what cocos
 *     polls to decide a sound has ended. */
static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = CONTAINER(self, Player, play_vt);
  SDL_LockMutex(p->lock);

  switch (state) {
    case SL_PLAYSTATE_STOPPED:
      p->playing = 0;
      if (p->is_decoded) {          /* rewind */
        p->cur = NULL;
        p->cur_fpos = 0.0;
        p->finished = 0;
        p->drained = 0;
      }
      break;

    case SL_PLAYSTATE_PAUSED:
      p->playing = 0;               /* position retained deliberately */
      break;

    case SL_PLAYSTATE_PLAYING:
      if (p->is_decoded && p->finished) {
        p->cur = NULL;              /* played out: start again from the top */
        p->cur_fpos = 0.0;
        p->finished = 0;
        p->drained = 0;
      }
      p->playing = 1;
      break;

    default:
      break;
  }
  p->state = state;
  SDL_UnlockMutex(p->lock);

  pf_announce_ready(p);
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (!pState) return SL_RESULT_PARAMETER_INVALID;
  /* Report what was asked for, except that a source which has played out is
   * genuinely stopped however it was last set. */
  if (p->is_decoded && p->finished)      *pState = SL_PLAYSTATE_STOPPED;
  else if (p->state)                     *pState = p->state;
  else                                   *pState = SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
/* Registering this was previously a no-op. A decoded source has to report
 * SL_PLAYEVENT_HEADATEND or cocos never learns the track finished, never
 * releases the player, and never starts the next one. */
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) {
  Player *p = CONTAINER(self, Player, play_vt);
  p->play_cb = (slPlayCallback)cb;
  p->play_cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}
static SLresult play_SetCallbackEventsMask(void *self, SLuint32 mask) {
  Player *p = CONTAINER(self, Player, play_vt);
  p->play_events = mask;
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetCallbackEventsMask(void *self, SLuint32 *pMask) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (pMask) *pMask = p->play_events;
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetDuration(void *self, SLuint32 *pMsec) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
  *pMsec = (p->is_decoded && p->rate > 0)
             ? (SLuint32)((p->total_frames * 1000ull) / (unsigned)p->rate)
             : 0;
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPosition(void *self, SLuint32 *pMsec) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
  *pMsec = (p->is_decoded && p->rate > 0)
             ? (SLuint32)(((uint64_t)p->cur_fpos * 1000ull) / (unsigned)p->rate)
             : 0;
  return SL_RESULT_SUCCESS;
}

static const SLPlayItf_ play_vtable = {
  play_SetPlayState, play_GetPlayState, play_GetDuration, play_GetPosition,
  play_RegisterCallback, play_SetCallbackEventsMask, play_GetCallbackEventsMask, play_ok_u32,
  play_ok, play_ret0_u32, play_ok_u32, play_ret0_u32,
};

/* Seek: cocos loops background music through SetLoop rather than by
 * re-issuing the sound. */
static SLresult seek_SetPosition(void *self, SLuint32 pos, SLuint32 mode) {
  Player *p = CONTAINER(self, Player, seek_vt);
  (void)mode;
  if (!p->is_decoded || p->rate <= 0) return SL_RESULT_SUCCESS;
  SDL_LockMutex(p->lock);
  double frame = ((double)pos / 1000.0) * (double)p->rate;
  if (frame < 0) frame = 0;
  if (frame > (double)p->total_frames) frame = (double)p->total_frames;
  p->cur      = (const uint8_t *)p->own_pcm;
  p->cur_size = p->own_bytes;
  p->cur_fpos = frame;
  p->finished = 0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult seek_SetLoop(void *self, SLboolean enable, SLuint32 start, SLuint32 end) {
  Player *p = CONTAINER(self, Player, seek_vt);
  (void)start; (void)end;
  p->loop = enable ? 1 : 0;
  return SL_RESULT_SUCCESS;
}
static SLresult seek_GetLoop(void *self, SLboolean *pEnable, SLuint32 *pStart, SLuint32 *pEnd) {
  Player *p = CONTAINER(self, Player, seek_vt);
  if (pEnable) *pEnable = p->loop ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  if (pStart)  *pStart = 0;
  if (pEnd)    *pEnd = 0;
  return SL_RESULT_SUCCESS;
}
static const SLSeekItf_ seek_vtable = { seek_SetPosition, seek_SetLoop, seek_GetLoop };

/* Prefetch status.
 *
 * UrlAudioPlayer::prepare() requests this interface. The whole file is already
 * decoded and resident by the time the player exists, so the honest answer to
 * every question is "buffered, nothing pending". The callback is fired once,
 * because a caller that waits for prefetch to complete would otherwise wait
 * forever. */
static SLresult pf_GetPrefetchStatus(void *self, SLuint32 *pStatus) {
  (void)self;
  if (pStatus) *pStatus = SL_PREFETCHSTATUS_SUFFICIENTDATA;
  return SL_RESULT_SUCCESS;
}
static SLresult pf_GetFillLevel(void *self, SLuint32 *pLevel) {
  (void)self;
  if (pLevel) *pLevel = 1000;   /* permille: completely filled */
  return SL_RESULT_SUCCESS;
}
static SLresult pf_RegisterCallback(void *self, slPrefetchCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, prefetch_vt);
  p->prefetch_cb = cb;
  p->prefetch_cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}
static SLresult pf_SetCallbackEventsMask(void *self, SLuint32 mask) {
  Player *p = CONTAINER(self, Player, prefetch_vt);
  p->prefetch_events = mask;
  return SL_RESULT_SUCCESS;
}

/* Reported once, from the first SetPlayState rather than from inside
 * SetCallbackEventsMask. Firing from the setter re-enters cocos in the middle
 * of its own player setup, which is a needless hazard for something that only
 * has to be said once and is true from the moment the player exists. */
static void pf_announce_ready(Player *p) {
  if (!p->prefetch_cb || p->prefetch_announced) return;
  p->prefetch_announced = 1;
  SLuint32 ev = p->prefetch_events
                  ? p->prefetch_events
                  : (SL_PREFETCHEVENT_STATUSCHANGE | SL_PREFETCHEVENT_FILLLEVELCHANGE);
  p->prefetch_cb(&p->prefetch_vt, p->prefetch_cb_ctx, ev);
}
static SLresult pf_GetCallbackEventsMask(void *self, SLuint32 *pMask) {
  (void)self;
  if (pMask) *pMask = SL_PREFETCHEVENT_STATUSCHANGE | SL_PREFETCHEVENT_FILLLEVELCHANGE;
  return SL_RESULT_SUCCESS;
}
static SLresult pf_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult pf_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLPrefetchStatusItf_ prefetch_vtable = {
  pf_GetPrefetchStatus, pf_GetFillLevel, pf_RegisterCallback,
  pf_SetCallbackEventsMask, pf_GetCallbackEventsMask,
  pf_ok_u32, pf_ret0_u32,
};

/* Last-resort table for interfaces this port does not implement. Returning
 * NULL from GetInterface is correct by the spec, but a caller that skips the
 * result check and dereferences anyway then jumps through address 0. Pointing
 * at no-op stubs turns that crash into a call that does nothing. */
static SLresult generic_noop(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static void *const generic_itf_vtable[24] = {
  (void *)generic_noop, (void *)generic_noop, (void *)generic_noop, (void *)generic_noop,
  (void *)generic_noop, (void *)generic_noop, (void *)generic_noop, (void *)generic_noop,
  (void *)generic_noop, (void *)generic_noop, (void *)generic_noop, (void *)generic_noop,
  (void *)generic_noop, (void *)generic_noop, (void *)generic_noop, (void *)generic_noop,
  (void *)generic_noop, (void *)generic_noop, (void *)generic_noop, (void *)generic_noop,
  (void *)generic_noop, (void *)generic_noop, (void *)generic_noop, (void *)generic_noop,
};
static const void *generic_itf = generic_itf_vtable;

/* Volume. */

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  Player *p = CONTAINER(self, Player, vol_vt);
  // Clamp to the OpenSL volume range before converting.
  int mb = (int)level;
  if (mb > 0) mb = 0;
  if (mb < -9600) mb = -9600;
  p->gain = mb_to_linear(mb);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_SetMute(void *self, SLboolean m) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (m) p->gain = 0.0f;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_enable(void *self, SLboolean e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_isenabled(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setpos(void *self, SLint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_getpos(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLVolumeItf_ vol_vtable = {
  vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute,
  vol_GetMute, vol_enable, vol_isenabled, vol_setpos, vol_getpos,
};

/* Playback-rate properties.
 *
 * SL_RATEPROP_NOPITCHCORAUDIO: rate changes shift pitch. That is exactly what
 * the resampler in mix_player does, and what a game wants for SFX variation. */
#define SL_RATEPROP_NOPITCHCORAUDIO ((SLuint32)0x2)

static SLresult rate_SetRate(void *self, SLint16 r) {
  Player *p = CONTAINER(self, Player, rate_vt);
  /* Clamp to the range advertised by rate_GetRange. Outside it the spec says
   * to fail rather than silently saturate, and a rate of 0 would freeze the
   * read cursor and hang the voice forever. */
  if (r < 500 || r > 2000) return SL_RESULT_PARAMETER_INVALID;
  p->play_rate = r;
  return SL_RESULT_SUCCESS;
}
static SLresult rate_GetRate(void *self, SLint16 *out) {
  Player *p = CONTAINER(self, Player, rate_vt);
  if (out) *out = p->play_rate ? p->play_rate : 1000;
  return SL_RESULT_SUCCESS;
}
static SLresult rate_SetProps(void *self, SLuint32 c) { (void)self; (void)c; return SL_RESULT_SUCCESS; }
static SLresult rate_GetProps(void *self, SLuint32 *p) { (void)self; if (p) *p = SL_RATEPROP_NOPITCHCORAUDIO; return SL_RESULT_SUCCESS; }
static SLresult rate_GetCaps(void *self, SLuint32 *p) {
  (void)self; if (p) *p = SL_RATEPROP_NOPITCHCORAUDIO; return SL_RESULT_SUCCESS;
}
static SLresult rate_GetRange(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop) {
  (void)self; (void)i;
  if (min) *min = 500;
  if (max) *max = 2000;
  if (step) *step = 1;
  if (prop) *prop = SL_RATEPROP_NOPITCHCORAUDIO;
  return SL_RESULT_SUCCESS;
}
static const SLPlaybackRateItf_ rate_vtable = {
  rate_SetRate, rate_GetRate, rate_SetProps, rate_GetProps, rate_GetCaps, rate_GetRange,
};

/* Android configuration properties. */

static SLresult cfg_SetConfiguration(void *self, const void *key, const void *value, SLuint32 sz) {
  (void)self; (void)key; (void)value; (void)sz; return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(void *self, const void *key, SLuint32 *psz, void *value) {
  (void)self; (void)key; (void)value; if (psz) *psz = 0; return SL_RESULT_SUCCESS;
}
static SLresult cfg_AcquireJavaProxy(void *self, SLuint32 t, void *p) {
  (void)self; (void)t; if (p) *(void **)p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult cfg_ReleaseJavaProxy(void *self, SLuint32 t) { (void)self; (void)t; return SL_RESULT_SUCCESS; }

static const SLAndroidConfigurationItf_ cfg_vtable = {
  cfg_SetConfiguration, cfg_GetConfiguration, cfg_AcquireJavaProxy, cfg_ReleaseJavaProxy,
};

/* Player object. */

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void player_Destroy(void *self);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult obj_Abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult obj_SetPriority(void *self, SLint32 a, SLboolean b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLOC(void *self, SLint32 a, SLInterfaceID *b, SLboolean c) { (void)self; (void)a; (void)b; (void)c; return SL_RESULT_SUCCESS; }

static SLresult mix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  (void)self; (void)iid;
  if (pInterface) *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void simple_Destroy(void *self) { free(self); }

static const SLObjectItf_ player_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback,
  obj_Abort, player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};
static const SLObjectItf_ mix_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) {
    *(void **)pInterface = &p->play_vt;
  } else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)pInterface = &p->bq_vt;
  } else if (iid == SL_IID_VOLUME) {
    *(void **)pInterface = &p->vol_vt;
  } else if (iid == SL_IID_PLAYBACKRATE) {
    *(void **)pInterface = &p->rate_vt;
  } else if (iid == SL_IID_ANDROIDCONFIGURATION) {
    *(void **)pInterface = &p->config_vt;
  } else if (iid == SL_IID_SEEK) {
    *(void **)pInterface = &p->seek_vt;
  } else if (iid == SL_IID_PREFETCHSTATUS) {
    *(void **)pInterface = &p->prefetch_vt;
  } else {
    *(void **)pInterface = (void *)&generic_itf;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == p) g_players[i] = NULL;
  SDL_UnlockMutex(g_reg_lock);
  if (p->lock) SDL_DestroyMutex(p->lock);
  if (p->is_decoded && g_decoded_live > 0) g_decoded_live--;
  free(p->own_pcm);
  free(p);
}

/* Engine. */

#define SL_DATALOCATOR_ANDROIDFD 0x800007BCu
#define SL_DATAFORMAT_MIME       0x00000001u
typedef int64_t SLAint64;
#define SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE ((SLAint64)0xFFFFFFFFFFFFFFFFull)

typedef struct {
  SLuint32 locatorType;
  SLint32  fd;
  SLAint64 offset;
  SLAint64 length;
} SLDataLocator_AndroidFD;

/* Pull the region the locator describes out of the descriptor and decode it.
 * The descriptor belongs to cocos -- it closes it when the player goes away --
 * so the file position is restored rather than left where reading finished. */
/* Serialises descriptor reads. The file position is state shared with whoever
 * owns the descriptor, so two decodes running at once -- the same sound
 * started twice, or two streams together -- would seek each other's reads out
 * from under them and both get garbage. Decodes are rare enough that a plain
 * mutex costs nothing. */
static SDL_mutex *g_decode_lock;

static int read_and_decode_fd(const SLDataLocator_AndroidFD *loc,
                              int16_t **pcm, size_t *frames,
                              int *rate, int *channels) {
  if (!loc || loc->fd < 0) return 0;

  SDL_LockMutex(g_decode_lock);
  off_t saved = lseek(loc->fd, 0, SEEK_CUR);

  off_t length = (off_t)loc->length;
  if (loc->length == SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE || length <= 0) {
    off_t end = lseek(loc->fd, 0, SEEK_END);
    length = end - (off_t)loc->offset;
  }
  if (length <= 0 || length > (64 << 20)) {
    debugLogNote("[sl] descriptor length %lld is not usable\n", (long long)length);
    if (saved >= 0) lseek(loc->fd, saved, SEEK_SET);
    SDL_UnlockMutex(g_decode_lock);
    return 0;
  }

  uint8_t *buf = malloc((size_t)length);
  if (!buf) {
    debugLogNote("[sl] could not allocate %lld bytes to read the stream\n", (long long)length);
    if (saved >= 0) lseek(loc->fd, saved, SEEK_SET);
    SDL_UnlockMutex(g_decode_lock);
    return 0;
  }

  lseek(loc->fd, (off_t)loc->offset, SEEK_SET);
  size_t got = 0;
  while (got < (size_t)length) {
    ssize_t n = read(loc->fd, buf + got, (size_t)length - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  if (saved >= 0) lseek(loc->fd, saved, SEEK_SET);

  SDL_UnlockMutex(g_decode_lock);   /* descriptor done with; decode freely */

  if (got != (size_t)length) {
    debugLogNote("[sl] short read from descriptor: %u of %lld bytes\n",
                 (unsigned)got, (long long)length);
    free(buf);
    return 0;
  }

  const int ok = mp3_decode_buffer(buf, got, pcm, frames, rate, channels);
  free(buf);
  if (!ok)
    debugLogNote("[sl] descriptor contents are not decodable MP3 -- staying silent\n");
  return ok;
}

static SLresult eng_CreateAudioPlayer(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req) {
  {
    /* The single most useful audio diagnostic: which locator/format the game
     * asked for. cocos uses a PCM buffer queue for decoded sound effects and
     * an Android FD source for streamed music; only the first is supported. */
    SLuint32 srcLoc = (src && src->pLocator) ? *(SLuint32 *)src->pLocator : 0xFFFFFFFFu;
    SLuint32 srcFmt = (src && src->pFormat)  ? *(SLuint32 *)src->pFormat  : 0xFFFFFFFFu;
    SLuint32 snkLoc = (snk && snk->pLocator) ? *(SLuint32 *)snk->pLocator : 0xFFFFFFFFu;
    debugLogNote("[sl] CreateAudioPlayer src=0x%X fmt=0x%X sink=0x%X\n",
                 (unsigned)srcLoc, (unsigned)srcFmt, (unsigned)snkLoc);
  }
  (void)self; (void)numIfaces; (void)ids; (void)req;
  if (!pPlayer)
    return SL_RESULT_PARAMETER_INVALID;

  /* Refuse sources this layer cannot possibly render.
   *
   * cocos hands us SL_DATALOCATOR_ANDROIDFD (0x800007BC) with
   * SL_DATAFORMAT_MIME whenever it wants the *platform* to decode a
   * compressed file -- either to stream it, or to decode it to PCM through a
   * buffer-queue sink. There is no decoder here, so such a player can never
   * produce audio and can never reach end-of-stream.
   *
   * Accepting it anyway was worse than useless: cocos keeps the file
   * descriptor it obtained from AAsset_openFileDescriptor() and the player
   * object alive until playback completes, which never happened. Every sound
   * leaked one fd and one of the MAX_PLAYERS slots. That is why the game ran
   * correctly for a while and then failed once enough sounds had played --
   * the log shows 844 of these in a single session against 64 slots.
   *
   * Failing here lets cocos close the fd, release the player and move on. */
  /* An SL_DATALOCATOR_ANDROIDFD source with SL_DATAFORMAT_MIME is cocos asking
   * the platform to decode a compressed file for it -- the path it takes for
   * anything too large to preload. Decode it here and hand the mixer a normal
   * PCM source; see mp3_decode.c for why this rather than forcing cocos to
   * preload. */
  int16_t *decoded_pcm = NULL;
  size_t   decoded_frames = 0;
  int      decoded_rate = 0, decoded_channels = 0;
  {
    const SLuint32 srcLocator = (src && src->pLocator) ? *(SLuint32 *)src->pLocator : 0;
    const SLuint32 srcFormat  = (src && src->pFormat)  ? *(SLuint32 *)src->pFormat  : 0;
    if (srcLocator == SL_DATALOCATOR_ANDROIDFD || srcFormat == SL_DATAFORMAT_MIME) {
      if (!config.decode_stream_audio) {
        static int noted = 0;
        if (!noted) { noted = 1; debugLogNote("[sl] stream decoding disabled by config\n"); }
        return SL_RESULT_CONTENT_UNSUPPORTED;
      }
      if (srcLocator != SL_DATALOCATOR_ANDROIDFD) {
        debugLogNote("[sl] MIME source without a descriptor -- cannot decode\n");
        return SL_RESULT_CONTENT_UNSUPPORTED;
      }
      const SLDataLocator_AndroidFD *fdloc = (const SLDataLocator_AndroidFD *)src->pLocator;
      if (!read_and_decode_fd(fdloc, &decoded_pcm, &decoded_frames,
                              &decoded_rate, &decoded_channels))
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
  }
  (void)snk;

  {
    static int announced = 0;
    if (!announced) {
      announced = 1;
      debugLogNote("[sl] first PCM player accepted -- decoded audio path is live\n");
    }
  }

  Player *p = calloc(1, sizeof(*p));
  if (!p) {
    free(decoded_pcm);
    return SL_RESULT_PARAMETER_INVALID;
  }
  p->obj_vt = &player_obj_vtable;
  p->play_vt = &play_vtable;
  p->bq_vt = &bq_vtable;
  p->vol_vt = &vol_vtable;
  p->rate_vt = &rate_vtable;
  p->play_rate = 1000;          /* normal speed until SetRate says otherwise */
  p->config_vt = &cfg_vtable;
  p->seek_vt = &seek_vtable;
  p->prefetch_vt = &prefetch_vtable;
  p->in_use = 1;
  p->gain = 1.0f;
  p->channels = 2;
  p->rate = 44100;
  p->sbytes = 2;     // assume 16-bit signed PCM unless the format says otherwise
  p->is_float = 0;
  p->lock = SDL_CreateMutex();

  if (decoded_pcm) {
    const uint64_t bytes =
        (uint64_t)decoded_frames * (uint64_t)decoded_channels * sizeof(int16_t);
    if (bytes > 0xF0000000ull) {
      /* own_bytes and cur_size are SLuint32. A track this long is not real,
       * but truncating the size the mixer reads from would be silent
       * corruption rather than an error. */
      debugLogNote("[sl] decoded stream is %llu MB -- too large to address\n",
                   (unsigned long long)(bytes / 1048576));
      free(decoded_pcm);
      decoded_pcm = NULL;
    }
  }
  if (decoded_pcm) {
    /* Set after the defaults above, not before: they are assigned
     * unconditionally and would otherwise clobber the decoded format. It only
     * looked correct because this track happens to be 44100/stereo, which is
     * exactly what the defaults say. */
    p->is_decoded   = 1;
    p->own_pcm      = decoded_pcm;
    p->total_frames = decoded_frames;
    p->own_bytes    = (SLuint32)(decoded_frames * (size_t)decoded_channels * sizeof(int16_t));
    p->channels     = decoded_channels;
    p->rate         = decoded_rate;
    p->sbytes       = 2;
    p->is_float     = 0;
    g_decoded_live++;
    debugLogNote("[sl] decoded source ready: %d Hz, %d ch, %u frames (%d live)\n",
                 decoded_rate, decoded_channels, (unsigned)decoded_frames,
                 g_decoded_live);
    if (g_decoded_live > 4)
      debugLogNote("[sl] WARNING: %d decoded sources alive at once -- cocos is "
                   "not destroying them, each holds its whole PCM buffer\n",
                   g_decoded_live);
  } else if (src && src->pFormat) {
    const SLDataFormat_PCM *fmt = src->pFormat;
    // formatType 2 = SL_DATAFORMAT_PCM (integer); 4 = SL_ANDROID_DATAFORMAT_PCM_EX
    // (adds a trailing 'representation' field: 1=signed int, 2=float, 3=unsigned).
    if (fmt->formatType == 2 || fmt->formatType == 4) {
      p->channels = fmt->numChannels ? (int)fmt->numChannels : 2;
      p->rate = fmt->samplesPerSec ? (int)(fmt->samplesPerSec / 1000) : 44100;
      // stride is the container size (bits) when given, else the sample width.
      uint32_t stride_bits = fmt->containerSize ? fmt->containerSize : fmt->bitsPerSample;
      p->sbytes = stride_bits >= 32 ? 4 : 2;
      if (fmt->formatType == 4) {
        uint32_t representation = ((const uint32_t *)fmt)[7]; // field after endianness
        p->is_float = (representation == 2);
      }
    }
  }

  ensure_device();

  SDL_LockMutex(g_reg_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot < 0) {
    // Reclaim a finished one-shot while the mixer lock is held.
    for (int i = 0; i < g_player_count; i++) {
      Player *q = g_players[i];
      /* Reclaiming frees the object while cocos may still hold the SLObjectItf
       * it was handed. That is survivable for an abandoned one-shot buffer
       * queue, which is what this rule was written for.
       *
       * It is NOT survivable for a decoded source. cocos keeps the player
       * alive until its own end-of-stream callback runs and calls Destroy, so
       * reclaiming a finished one hands it a dangling object -- and the damage
       * appears later, somewhere unrelated. An earlier version of this did
       * exactly that, and the decoded music player is precisely the kind that
       * sits "finished" for a while before cocos gets round to it. */
      const int spent = q && !q->is_decoded && q->playing && q->drained > 40;
      if (spent) {
        debugLogNote("[sl] reclaiming player slot %d from a drained one-shot\n", i);
        g_players[i] = NULL;
        if (q->lock) SDL_DestroyMutex(q->lock);
        free(q->own_pcm);
        free(q);
        slot = i;
        break;
      }
    }
  }
  if (slot >= 0)
    g_players[slot] = p;
  SDL_UnlockMutex(g_reg_lock);

  if (slot < 0) {
    /* All MAX_PLAYERS slots are live and none were reclaimable. Returning
     * success here would hand back a player the mixer never sees: silent,
     * and leaked for the rest of the session. */
    if (p->lock) SDL_DestroyMutex(p->lock);
    free(p->own_pcm);   /* a decoded source owns megabytes; do not drop it here */
    free(p);
    return SL_RESULT_RESOURCE_ERROR;
  }

  *pPlayer = &p->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, SLObjectItf *pMix, SLuint32 numIfaces,
                                    const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)numIfaces; (void)ids; (void)req;
  if (!pMix)
    return SL_RESULT_PARAMETER_INVALID;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m)
    return SL_RESULT_PARAMETER_INVALID;
  m->obj_vt = &mix_obj_vtable;
  *pMix = &m->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const SLEngineItf_ engine_vtable = {
  .CreateLEDDevice = (void *)eng_unsupported,
  .CreateVibraDevice = (void *)eng_unsupported,
  .CreateAudioPlayer = eng_CreateAudioPlayer,
  .CreateAudioRecorder = (void *)eng_unsupported,
  .CreateMidiPlayer = (void *)eng_unsupported,
  .CreateListener = (void *)eng_unsupported,
  .Create3DGroup = (void *)eng_unsupported,
  .CreateOutputMix = eng_CreateOutputMix,
  .CreateMetadataExtractor = (void *)eng_unsupported,
  .CreateExtensionObject = (void *)eng_unsupported,
  .QueryNumSupportedInterfaces = (void *)eng_unsupported,
  .QuerySupportedInterfaces = (void *)eng_unsupported,
  .QueryNumSupportedExtensions = (void *)eng_unsupported,
  .QuerySupportedExtension = (void *)eng_unsupported,
  .IsExtensionSupported = (void *)eng_unsupported,
};

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)pInterface = &e->eng_vt;
    return SL_RESULT_SUCCESS;
  }
  *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLObjectItf_ engine_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

/* Entry point. */

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired) {
  debugLogNote("[sl] slCreateEngine\n");
  /* Created here rather than on first use: engine creation is single-threaded,
   * whereas a lazy "if (!lock) create" would itself be a race. */
  if (!g_decode_lock) g_decode_lock = SDL_CreateMutex();
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (!pEngine)
    return SL_RESULT_PARAMETER_INVALID;
  Engine *e = calloc(1, sizeof(*e));
  if (!e)
    return SL_RESULT_PARAMETER_INVALID;
  e->obj_vt = &engine_obj_vtable;
  e->eng_vt = &engine_vtable;
  *pEngine = &e->obj_vt;
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  if (g_dev) {
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
  }
}

void opensles_ensure_output(void) {
  ensure_device();
}
