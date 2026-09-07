/* dfu_keyboard.c -- Switch software keyboard for Daggerfall's text boxes.
 *
 * THE PROBLEM. Naming a character brings up a DaggerfallWorkshop TextBox, which
 * is a classic-style box that polls DFU's own input state. It does NOT use
 * UnityEngine.TouchScreenKeyboard -- every TouchScreenKeyboard reference in
 * dump.cs belongs to UnityEngine.UIElements -- so Unity never calls the Java
 * showSoftInput path, and the swkbd plumbing in editbox.c (which is real, and
 * identical to PvZ's) never had anything to trigger it. The boot log shows
 * exactly that: UnityPlayer.hideSoftInput gets resolved, showSoftInput never
 * does.
 *
 * THE FOCUS SIGNAL, from TextBox::Update() at il2cpp+0x1f9b264:
 *
 *     ldrb w8, [x19, #0x3b0]        ; this->readOnly  -> if set, return
 *     ldrb w8, [x19, #0x48]
 *     bl   BaseScreenComponent::HasFocus()
 *     tbz  w0, #0, ...ret           ; no focus -> return
 *     ...
 *     bl   TextBox::HandleCharacterInput()
 *
 * So HandleCharacterInput is reached ONLY for a text box that is both writable
 * and focused. Being called IS the focus signal -- no polling, no guessing at
 * window stacks, and `this` arrives in x0. That is why this file hooks that
 * function rather than anything higher up.
 *
 * WHY A TRAMPOLINE. Every other hook in this port REPLACES a function. Here the
 * original must still run, because it is what actually types. Its first four
 * instructions are
 *     sub sp, sp, #0x40 ; stp x30,x23,[sp,#0x10] ; stp x22,x21 ; stp x20,x19
 * -- entirely PC-independent, verified against the shipped binary, so they can
 * be relocated. tools/verify_offsets.py re-checks that; if a future build opens
 * with an adrp or a branch the guard fails and the hook is skipped rather than
 * corrupting the function.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <switch.h>

#include "util.h"
#include "config.h"
#include "so_util.h"
#include "dfu_offsets.h"
#include "editbox.h"
#include "nx_pointer.h"

/* ---- resolved at init from il2cpp ---------------------------------------- */
typedef void  (*fn_handle_char)(void *self, void *mi);
typedef void  (*fn_set_text)(void *self, void *str, void *mi);
typedef void *(*fn_string_new)(const char *utf8);

static fn_handle_char g_orig_handle_char;   /* the trampoline, not the hook */
static fn_set_text    g_set_text;
static fn_string_new  g_string_new;

static int   g_ready;
static void *g_focus_box;                   /* the TextBox taking characters  */
static volatile uint32_t g_focus_frame;     /* port frame it was last seen on */
static uint32_t g_last_open_frame;

extern uint32_t port_frame_count(void);

/* The trampoline needs to be EXECUTABLE, and this port's own .bss is not --
 * homebrew data pages are RW. Only the loaded modules are executable, and they
 * got that way through svcMapProcessCodeMemory + Perm_Rx in so_util.c. There is
 * also no padding to borrow: HandleCharacterInput's 752 bytes run straight into
 * the next function with no alignment slack.
 *
 * So do what so_util does. Note the ordering constraint it documents: mapping
 * DONATES the source pages, and the source faults on access afterwards -- so
 * the trampoline must be written BEFORE the map, never after. */
static void *tramp_alloc_rx(const void *code, size_t len) {
  const size_t PAGE = 0x1000;
  /* __real_memalign, not memalign: this port wraps memalign and can serve it
   * from the GPU arena, which is a separate mapping and not something to hand
   * to svcMapProcessCodeMemory. Go straight to the C library. */
  extern void *__real_memalign(size_t align, size_t size);
  void *src = __real_memalign(PAGE, PAGE);
  if (!src) return NULL;
  memset(src, 0, PAGE);
  memcpy(src, code, len);                 /* WRITE FIRST -- see above */
  armDCacheFlush(src, PAGE);

  virtmemLock();
  void *va = virtmemFindCodeMemory(PAGE, PAGE);
  VirtmemReservation *rv = va ? virtmemAddReservation(va, PAGE) : NULL;
  virtmemUnlock();
  if (!va) { free(src); return NULL; }

  Handle self = envGetOwnProcessHandle();
  if (R_FAILED(svcMapProcessCodeMemory(self, (u64)va, (u64)src, PAGE))) {
    virtmemLock(); if (rv) virtmemRemoveReservation(rv); virtmemUnlock();
    extern void __real_free(void *p);
    __real_free(src);                     /* not donated: the map failed */
    return NULL;
  }
  if (R_FAILED(svcSetProcessMemoryPermission(self, (u64)va, PAGE, Perm_Rx))) {
    svcUnmapProcessCodeMemory(self, (u64)va, (u64)src, PAGE);
    virtmemLock(); if (rv) virtmemRemoveReservation(rv); virtmemUnlock();
    { extern void __real_free(void *p); __real_free(src); }
    return NULL;
  }
  /* src is donated now and must NOT be freed or touched. rv is kept for the
   * life of the process on purpose: the trampoline is never torn down. */
  armICacheInvalidate(va, PAGE);
  return va;
}

/* ---- the hook ------------------------------------------------------------ */
static void hk_HandleCharacterInput(void *self, void *mi) {
  g_focus_box   = self;
  g_focus_frame = port_frame_count();
  if (g_orig_handle_char) g_orig_handle_char(self, mi);
}

/* A text box is focused if the hook fired this frame or the one before. Two
 * frames of slack because the pad is pumped from the render loop and the UI
 * updates from the player loop, so a strict same-frame test would flicker. */
int dfu_kbd_textbox_focused(void) {
  return g_ready && g_focus_box &&
         (port_frame_count() - g_focus_frame) <= 2u;
}

/* Open the Switch keyboard for the focused box and write the result back.
 * Returns 1 if it ran. Blocking: swkbdShow runs the system applet, so this must
 * be called from the render loop, never from audio or the clock thread. */
int dfu_kbd_open(void) {
  if (!dfu_kbd_textbox_focused() || !g_set_text || !g_string_new) return 0;
  /* One open per press, and never twice in the same handful of frames -- the
   * applet returns with the button still logically down. */
  if (port_frame_count() - g_last_open_frame < 30u) return 0;
  g_last_open_frame = port_frame_count();

  void *box = g_focus_box;                 /* snapshot: the hook may re-fire */

  /* swkbdShow() blocks the render thread for as long as the applet is up, so
   * the input pump stops running -- and anything held at that instant stays
   * held for the whole time the keyboard is on screen and after it closes.
   * Let go of everything first. */
  { extern void dfu_input_release_all(void); dfu_input_release_all(); }
  nxp_release_touch();   /* and the pointer's finger, for the same reason */

  editbox_show("", DFU_KBD_MAXLEN);        /* blocking; fills the result */

  /* editbox_text() returns the SEED on cancel, not NULL, so it cannot be used
   * to detect backing out -- ask the flag. Writing the seed back would wipe
   * whatever the player had already typed. */
  if (editbox_cancelled()) { debugPrintf("[kbd] cancelled; TextBox untouched\n"); return 1; }
  const char *text = editbox_text();
  if (!text) return 1;

  void *s = g_string_new(text);
  if (s) {
    g_set_text(box, s, NULL);
    debugPrintf("[kbd] typed %d chars into TextBox %p\n", (int)strlen(text), box);
  }
  return 1;
}

/* Per-frame, from the RENDER loop only (swkbdShow blocks in the system applet).
 *
 * Opens automatically the first time a text box takes focus, which is what
 * "the keyboard should pop up" means. `Y` reopens it, because cancelling is a
 * normal thing to do and otherwise there would be no way back without leaving
 * the screen and returning. */
void dfu_kbd_tick(void) {
  /* Samples its OWN pad rather than taking the bridge's button word. Text entry
   * must not stop working because the pad bridge guard-checked out: they fail
   * for unrelated reasons, and a player who cannot name a character is stuck in
   * a way a player without pad bindings is not. A second PadState only reads
   * state, it does not consume it. */
  static PadState kpad;
  static int kpad_ready;
  if (!kpad_ready) { padInitializeDefault(&kpad); kpad_ready = 1; }
  padUpdate(&kpad);
  const uint64_t buttons_pressed = padGetButtonsDown(&kpad);
  if (!g_ready) return;
  const int focused = dfu_kbd_textbox_focused();
  static int was_focused;

  if (focused && !was_focused) {           /* rising edge: a box just took focus */
    debugPrintf("[kbd] TextBox focused -> opening keyboard\n");
    /* Only consume the edge if the open actually ran. dfu_kbd_open() throttles
     * itself for 30 frames after the applet closes, and swallowing the edge on
     * a throttled call would leave the box focused with no keyboard and no way
     * to retry except Y. */
    if (dfu_kbd_open()) was_focused = 1;
    return;
  }
  if (!focused) { was_focused = 0; return; }
  if (buttons_pressed & HidNpadButton_Y) dfu_kbd_open();   /* reopen after cancel */
}

/* ---- install ------------------------------------------------------------- */
/* Takes the base as an ARGUMENT rather than reading g_il2cpp_base. The first
 * version read the global and was called from the Time-hook installer, which
 * runs ~250 lines BEFORE that global is assigned -- so the base was 0, the
 * guard fired, and the keyboard silently never installed. Passing it in makes
 * the dependency impossible to get wrong. */
void dfu_kbd_init(uintptr_t b) {
  if (!b) { debugPrintf("[kbd] il2cpp base is 0 -- keyboard NOT installed\n"); return; }

  /* Guard the prologue we are about to relocate. If it is not the four
   * PC-independent instructions this was written against, copying it would
   * move an adrp or a branch and corrupt the function -- refuse instead. */
  const uint32_t *p = (const uint32_t *)(b + OFF_TextBox_HandleCharacterInput);
  static const uint32_t want[4] = DFU_TEXTBOX_HCI_PROLOGUE;
  for (int i = 0; i < 4; i++) {
    if (p[i] != want[i]) {
      debugPrintf("[kbd] !! TextBox::HandleCharacterInput prologue MISMATCH at word %d "
                  "(have 0x%08x, want 0x%08x) -- keyboard NOT installed\n",
                  i, p[i], want[i]);
      return;
    }
  }

  g_set_text   = (fn_set_text)(b + OFF_TextBox_set_Text);
  g_string_new = (fn_string_new)(b + OFF_il2cpp_string_new);

  /* trampoline: the four original instructions, then an absolute jump back to
   * the fifth. Built in a normal buffer, then mapped executable. */
  uint8_t tr[32];
  const uint32_t jmp[2] = { 0x58000050u, 0xd61f0200u };   /* ldr x16,#8 ; br x16 */
  memcpy(tr + 0,  (const void *)p, 16);
  memcpy(tr + 16, jmp, 8);
  const uint64_t back = (uint64_t)(b + OFF_TextBox_HandleCharacterInput + 16);
  memcpy(tr + 24, &back, 8);

  void *rx = tramp_alloc_rx(tr, sizeof tr);
  if (!rx) {
    debugPrintf("[kbd] !! could not map an executable trampoline "
                "-- keyboard NOT installed (the hook is not applied, so the "
                "game is exactly as it was)\n");
    return;
  }
  g_orig_handle_char = (fn_handle_char)rx;

  /* now redirect the original at its entry */
  uint8_t stub[16];
  void *fn = (void *)hk_HandleCharacterInput;
  memcpy(stub + 0, jmp, 8);
  memcpy(stub + 8, &fn, 8);
  so_patch_code((void *)p, stub, sizeof stub);

  g_ready = 1;
  debugPrintf("[kbd] TextBox focus hook installed @il2cpp+0x%x "
              "(trampoline %p, set_Text @0x%x)\n",
              OFF_TextBox_HandleCharacterInput, rx,
              OFF_TextBox_set_Text);
}
