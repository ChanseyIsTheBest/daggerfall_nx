/* dfu_keyboard.h -- Switch software keyboard for Daggerfall's text boxes.
 * See dfu_keyboard.c for how focus is detected. */
#ifndef DFU_KEYBOARD_H
#define DFU_KEYBOARD_H
#include <stdint.h>

/* Install the TextBox focus hook. Call after libil2cpp is loaded and patched. */
void dfu_kbd_init(uintptr_t il2cpp_base);

/* Is a writable, focused Daggerfall TextBox taking characters right now? */
int  dfu_kbd_textbox_focused(void);

/* Open the software keyboard for it and write the result back. BLOCKING --
 * render thread only. Returns 1 if it ran. */
int  dfu_kbd_open(void);

/* Per-frame, render thread only. Opens automatically when a box takes focus;
 * `Y` reopens after a cancel. Samples its own pad, so it works
 * whether or not the Tier 2 pad bridge is enabled. */
void dfu_kbd_tick(void);

#endif
