/* editbox.c -- libnx software-keyboard bridge for the engine's EditBox/TextBox.
 *
 * Ported from the acpc_nx port. The engine drives text entry through JNI
 * ShowEditBox / IsOpenEditBox / GetEditBoxText / CloseEditBox: it renders its own
 * field and polls for the text, so all we have to do is pop the Switch software
 * keyboard when asked and hand the result back.
 *
 * swkbdShow() is BLOCKING (it runs the system applet), so editbox_show() does not
 * return until the user confirms or cancels. That means editbox_is_open() is only
 * ever observed as 0 by the engine -- which is correct: by the time it polls, the
 * keyboard has already closed and the text is ready. This is exactly how acpc_nx
 * behaves. Do NOT call this from a thread that must keep servicing the engine;
 * it is invoked from the JNI dispatch on the calling (game) thread, which is
 * stalled by the applet anyway.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <string.h>

#include "editbox.h"

extern int debugPrintf(char *text, ...);
extern unsigned g_kbd_trace;   /* jni_fake.c: log the next N object calls */
extern void kbd_push_result(const char *text, int cancelled);  /* jni_fake.c */

#define EDITBOX_TEXT_CAP 1024

static char g_editbox_text[EDITBOX_TEXT_CAP];
static int  g_editbox_open;

/* Launching a second library applet while one is already up takes the whole
 * console down -- an Atmosphere-level crash, not a process one. `g_editbox_open`
 * used to be guarded by a bare check-then-set, which is only safe if every
 * caller is on the same thread, and they are not:
 *
 *   jni_fake.c   Unity's own showSoftInput path, on Unity's JNI thread
 *   dfu_keyboard.c   the TextBox focus hook's auto-open, on the render thread
 *
 * Both can pass `if (g_editbox_open)` before either sets it. A TRY-lock, not a
 * blocking one: a caller that loses the race must give up immediately, never
 * queue a second keyboard behind the first. */
static Mutex g_editbox_lock;
static int  g_editbox_cancelled;

void editbox_show(const char *initial, int maxlen) {
  if (!mutexTryLock(&g_editbox_lock)) {     /* someone else already has it */
    debugPrintf("[kbd] editbox_show refused: a keyboard is already up "
                "(second applet would crash the console)\n");
    return;
  }
  g_editbox_open = 1;
  g_editbox_cancelled = 1;                  /* assume cancel until confirmed */

  if (!initial) initial = "";
  snprintf(g_editbox_text, sizeof g_editbox_text, "%s", initial);
  if (maxlen <= 0 || maxlen >= EDITBOX_TEXT_CAP) maxlen = EDITBOX_TEXT_CAP - 1;

  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_SUCCEEDED(rc)) {
    char result[EDITBOX_TEXT_CAP];
    snprintf(result, sizeof result, "%s", g_editbox_text);
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetInitialText(&kbd, g_editbox_text);
    swkbdConfigSetStringLenMax(&kbd, (u32)maxlen);
    rc = swkbdShow(&kbd, result, sizeof result);   /* blocks: system applet */
    if (R_SUCCEEDED(rc)) {
      snprintf(g_editbox_text, sizeof g_editbox_text, "%s", result);
      g_editbox_cancelled = 0;
    }
    swkbdClose(&kbd);
  }

  g_editbox_open = 0;
  mutexUnlock(&g_editbox_lock);
  debugPrintf("[kbd] swkbd closed: cancelled=%d text=\"%s\"\n",
              g_editbox_cancelled, g_editbox_text);
  g_kbd_trace = 60;   /* the retrieval call is in the next few dozen */
  /* Unity PUSHES soft-input results: its Java keyboard calls
   * nativeSetInputString + nativeSoftInputClosed. We are that Java side. */
  kbd_push_result(g_editbox_text, g_editbox_cancelled);
}

int editbox_is_open(void) { return g_editbox_open; }

/* Set the first time UNITY'S OWN soft-input path opens the keyboard. Once the
 * engine is driving, dfu_keyboard.c's TextBox auto-open must stand down: two
 * mechanisms racing for one applet is what crashed the console, and the engine
 * knows more about when a keyboard is wanted than a focus hook does. */
static int g_editbox_engine_driven;
void editbox_mark_engine_driven(void) { g_editbox_engine_driven = 1; }
int  editbox_engine_drives(void)      { return g_editbox_engine_driven; }

/* editbox_text() deliberately returns the SEEDED text on cancel, so callers
 * that just want "the current value" behave correctly. A caller that must
 * distinguish "user confirmed" from "user backed out" needs this instead. */
int editbox_cancelled(void) { return g_editbox_cancelled; }

const char *editbox_text(void) {
  /* On cancel, report the text unchanged from what the engine seeded us with,
   * which is what a cancelled edit should look like. */
  return g_editbox_text;
}

void editbox_close(void) {
  /* The applet owns the UI while it is up, so there is nothing to dismiss here;
   * record the cancel so a later editbox_text() reflects it. */
  if (g_editbox_open) g_editbox_cancelled = 1;
  (void)g_editbox_cancelled;
}
