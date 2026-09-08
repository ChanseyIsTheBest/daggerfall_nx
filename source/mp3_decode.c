/* mp3_decode.c -- decode a whole MP3 held in memory to interleaved S16 PCM.
 *
 * Exists because cocos hands compressed audio straight to the platform when it
 * decides a file is too big to preload: an SL_DATALOCATOR_ANDROIDFD source
 * with SL_DATAFORMAT_MIME, expecting the OS to decode it. Android has a
 * decoder behind that; this port did not, so those files were silent. Happy
 * Wheels puts exactly one file down that path, the menu music.
 *
 * Decoding here rather than forcing cocos to preload the file leaves the
 * game's own threading alone -- forcing the preload path crashed on boot.
 *
 * minimp3 is compiled without SIMD. The scalar path is plenty for a one-off
 * decode at load time, and it avoids depending on intrinsics that cannot be
 * test-compiled outside a devkitA64 build. Drop MINIMP3_NO_SIMD to turn NEON
 * back on if it ever matters.
 */

#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "mp3_decode.h"
#include "util.h"

int mp3_decode_buffer(const void *data, size_t size,
                      int16_t **out_pcm, size_t *out_frames,
                      int *out_rate, int *out_channels) {
  if (!data || size < 4 || !out_pcm || !out_frames) return 0;

  *out_pcm = NULL;
  *out_frames = 0;

  const u64 t0 = armGetSystemTick();

  mp3dec_t dec;
  mp3dec_init(&dec);

  const uint8_t *p = data;
  size_t left = size;

  /* Rough first guess at the output size so the buffer does not spend the
   * whole decode doubling: MP3 frames carry 1152 samples in about 400 bytes at
   * typical rates, so ~24 output samples per input byte is a fair start. */
  size_t cap_frames = (size / 400 + 1) * 1152;
  if (cap_frames < 4096) cap_frames = 4096;
  /* Only a starting point -- the buffer grows as needed. Left uncapped, a
   * large input asks for hundreds of megabytes in one go and fails on an
   * allocation it would never have filled. */
  if (cap_frames > (4u << 20)) cap_frames = 4u << 20;

  int channels = 0, rate = 0;
  int16_t *pcm = NULL;
  size_t frames = 0;

  int16_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  mp3dec_frame_info_t info;

  while (left > 0) {
    int samples = mp3dec_decode_frame(&dec, p, (int)left, frame_pcm, &info);

    if (info.frame_bytes <= 0) break;          /* no more syncable data */
    p    += info.frame_bytes;
    left -= (size_t)info.frame_bytes < left ? (size_t)info.frame_bytes : left;

    if (samples <= 0) continue;                /* ID3 or junk that was skipped */

    if (!channels) {
      channels = info.channels;
      rate     = info.hz;
      if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000) {
        free(pcm);
        return 0;
      }
      pcm = malloc(cap_frames * (size_t)channels * sizeof(int16_t));
      if (!pcm) return 0;
    } else if (info.channels != channels || info.hz != rate) {
      /* Mid-stream format change. Nothing here can handle it and no real
       * game asset does it; stop with what has been decoded so far. */
      break;
    }

    if (frames + (size_t)samples > cap_frames) {
      size_t want = cap_frames * 2;
      while (want < frames + (size_t)samples) want *= 2;
      int16_t *bigger = realloc(pcm, want * (size_t)channels * sizeof(int16_t));
      if (!bigger) { free(pcm); return 0; }
      pcm = bigger;
      cap_frames = want;
    }

    memcpy(pcm + frames * (size_t)channels, frame_pcm,
           (size_t)samples * (size_t)channels * sizeof(int16_t));
    frames += (size_t)samples;
  }

  if (!pcm || frames == 0) {
    free(pcm);
    return 0;
  }

  /* Hand back only what was used; this is the copy that stays resident. */
  int16_t *trimmed = realloc(pcm, frames * (size_t)channels * sizeof(int16_t));
  if (trimmed) pcm = trimmed;

  *out_pcm      = pcm;
  *out_frames   = frames;
  if (out_rate)     *out_rate     = rate;
  if (out_channels) *out_channels = channels;

  const double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1.0e6;
  debugLogNote("[mp3] decoded %u frames, %d Hz, %d ch (%.1f s audio, %.1f MB) in %.0f ms\n",
               (unsigned)frames, rate, channels,
               (double)frames / (double)(rate ? rate : 1),
               (double)(frames * (size_t)channels * sizeof(int16_t)) / 1048576.0, ms);
  return 1;
}
