#ifndef DFU_INPUT_H
#define DFU_INPUT_H
/* Keycode state the pad writes and the UnityEngine.Input hooks read.
 * See dfu_input.c -- managed code calls OUT to these; we never call in. */
int dfu_kc_is_down(int kc);
int dfu_kc_went_down(int kc);
int dfu_kc_went_up(int kc);
void dfu_kc_frame_end(void);

/* Drop every key/axis we are holding. Call whenever injection stops, or the
 * last state stays latched in the game forever. */
void dfu_input_release_all(void);

#endif
