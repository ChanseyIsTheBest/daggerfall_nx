/* Decode a whole MP3 held in memory to interleaved S16 PCM. See mp3_decode.c. */

#ifndef MP3_DECODE_H
#define MP3_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Returns 1 on success, with *out_pcm malloc'd (caller frees) and holding
 * *out_frames interleaved frames of *out_channels samples at *out_rate Hz.
 * Returns 0 and allocates nothing if the data is not decodable MP3. */
int mp3_decode_buffer(const void *data, size_t size,
                      int16_t **out_pcm, size_t *out_frames,
                      int *out_rate, int *out_channels);

#endif
