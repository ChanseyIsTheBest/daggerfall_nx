/* pps_input.c -- console input to onTouchEvent.
 *
 * MIT licensed. See LICENSE.
 *
 * nx_pointer owns the pad, touchscreen, mouse, gyro and the cursor overlay and
 * hands back a stream of (id, x, y, phase) events in render space. This file
 * is the thin translation from those onto the four touch types the King engine
 * expects, plus the Back button.
 *
 * TWO THINGS WORTH KNOWING
 * ------------------------
 * 1. The argument order is (id, type, x, y), NOT (type, id, x, y). That comes
 *    from the dex: GameView$5.run reads val$id into the first outgoing
 *    register and val$finalType into the second. The two are adjacent ints of
 *    the same type, so swapping them is invisible to the compiler and to the
 *    ABI -- it just delivers nonsense pointer ids and the game stops
 *    responding after the first tap.
 *
 * 2. The engine's touch types are its own, not Android's. GameView mapped
 *    ACTION_* down to four values before they reached native:
 *        DOWN 0   UP 1   MOVE 2   CANCEL 3
 *    with ACTION_POINTER_DOWN/UP folding into DOWN/UP and ACTION_OUTSIDE into
 *    MOVE. Passing raw Android constants would send ACTION_POINTER_DOWN (5) as
 *    a type the engine has no case for, and its switch falls through to
 *    "ignore".
 */

#include <switch.h>
#include <string.h>

#include "config.h"
#include "pps_input.h"
#include "nx_pointer.h"

static pps_touch_fn g_touch;
static void *g_env, *g_this;

/* Back is read from a PadState of its own, and that is not tidiness.
 * padGetButtonsDown is computed against each PadState's own previous snapshot,
 * so two readers sharing one state would have whichever polled second see no
 * edge at all -- the first read consumes it. nx_pointer owns its own pad for
 * the cursor; this is ours. */
static PadState g_back_pad;

void pps_input_init(pps_touch_fn touch, void *env, void *thiz) {
  g_touch = touch;
  g_env = env;
  g_this = thiz;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_back_pad);
}

/* Which pointer ids are currently down, so an UP can be matched to its DOWN
 * and a stale id never emits a MOVE the engine did not expect. */
static unsigned g_down_mask;

void pps_input_poll(void) {
  NxpEvent ev[16];
  int n, i;

  if (!g_touch) return;

  padUpdate(&g_back_pad);

  nxp_update();
  n = nxp_poll(ev, (int)(sizeof(ev) / sizeof(*ev)));

  for (i = 0; i < n; i++) {
    int type, x, y;
    const int id = ev[i].id;

    if (id < 0 || id >= 32) continue;

    switch (ev[i].phase) {
      case NXP_DOWN: type = PPS_TOUCH_DOWN; break;
      case NXP_UP:   type = PPS_TOUCH_UP;   break;
      case NXP_MOVE: type = PPS_TOUCH_MOVE; break;
      default:       continue;
    }

    /* Drop a MOVE or UP for a pointer that never went down. nx_pointer will
     * not normally produce one, but the cursor can be toggled off mid-stroke
     * with +, and the engine treats a MOVE on an unknown id as a drag from
     * wherever it last saw that id -- which is a flick across the board. */
    if (type != PPS_TOUCH_DOWN && !(g_down_mask & (1u << id)))
      continue;

    if (type == PPS_TOUCH_DOWN) g_down_mask |=  (1u << id);
    if (type == PPS_TOUCH_UP)   g_down_mask &= ~(1u << id);

    /* nx_pointer reports in render space already, because it was given
     * screen_w/screen_h at init. The engine was told the same size through
     * init() and updateScreenSize(), so no scaling belongs here -- adding any
     * would double-apply it. */
    x = (int)ev[i].x;
    y = (int)ev[i].y;

    if (x < 0) x = 0; else if (x >= PPS_RENDER_W) x = PPS_RENDER_W - 1;
    if (y < 0) y = 0; else if (y >= PPS_RENDER_H) y = PPS_RENDER_H - 1;

    LOGI("touch id=%d type=%d (%d,%d)", id, type, x, y);
    g_touch(g_env, g_this, id, type, x, y);
  }
}

/* Cancel every live pointer. Called when the app loses focus: otherwise the
 * engine keeps a finger down across a suspend and resumes mid-drag. */
void pps_input_cancel_all(void) {
  int id;
  if (!g_touch) return;
  for (id = 0; id < 32; id++) {
    if (g_down_mask & (1u << id))
      g_touch(g_env, g_this, id, PPS_TOUCH_CANCEL, 0, 0);
  }
  g_down_mask = 0;
}

int pps_input_back_pressed(void) {
  return (padGetButtonsDown(&g_back_pad) & HidNpadButton_B) != 0;
}

/* Holding + and - together for a moment is the "I want out" gesture. The game
 * has no quit of its own -- on Android the Back key eventually walked out of
 * the activity -- and a homebrew that can only be left by holding the home
 * button is unpleasant. */
int pps_input_quit_requested(void) {
  static int held_frames;
  const u64 mask = HidNpadButton_Plus | HidNpadButton_Minus;
  if ((padGetButtons(&g_back_pad) & mask) == mask) {
    if (++held_frames > 90) { held_frames = 0; return 1; }
  } else {
    held_frames = 0;
  }
  return 0;
}
