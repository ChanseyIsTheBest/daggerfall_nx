/* dfu_input.c -- Switch pad -> Daggerfall Unity action bridge.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * ---------------------------------------------------------------------------
 * DESIGN
 * ---------------------------------------------------------------------------
 * Daggerfall Unity's Android build drives its 45 Actions and 4 AxisActions
 * through TouchscreenInputManager, which exposes two public static injection
 * points (see dfu_input_offsets.h for the derivation and the ABI check). So
 * this file does not synthesise Android input events at all. It calls the
 * game's own input system.
 *
 * There are two tiers, and BOTH are live at once -- they compose rather than
 * exclude, exactly as touch and pad compose on an Android device with a
 * controller attached:
 *
 *   Tier 1  TOUCH PASSTHROUGH.  The game's on-screen controls are driven by
 *           the ordinary touch path the PvZ base already implements
 *           (nx_pointer.c -> jni_fake MotionEvent -> nativeInjectEvent). In
 *           handheld this is real touch. Nothing in this file is involved, and
 *           nothing here breaks it: we only ever ADD action state.
 *
 *   Tier 2  PAD INJECTION.  Sticks and buttons are pushed straight into
 *           TouchscreenInputManager. This is what makes docked play work and
 *           gives analogue movement and look.
 *
 * Because Tier 2 only sets state and never clears the touch layer's, a player
 * can use the on-screen controls and the pad interchangeably.
 *
 * ---------------------------------------------------------------------------
 * THE BINDING PROBLEM, AND HOW THIS AVOIDS GUESSING
 * ---------------------------------------------------------------------------
 * SetAxis takes an AxisAction directly, so the sticks are unambiguous.
 *
 * SetKey, however, takes a KeyCode -- not an Action -- because that is how the
 * on-screen buttons work: each TouchscreenButton carries both `myAction` and
 * `myKey`, and InputManager resolves KeyCode -> Action through the player's
 * (rebindable) bindings. So injecting a button press needs the KeyCode the
 * player currently has bound to the action we mean.
 *
 * Hardcoding a table of Daggerfall's stock defaults would be guessing, and it
 * would silently break for anyone who rebinds. Instead we ask the game:
 *
 *     InputManager.Instance.GetDefaultBinding(action) -> KeyCode
 *
 * resolved once, lazily, on the first frame the instance exists, and cached.
 * If the instance is not up yet we simply do not inject that frame; movement
 * axes still work because they need no binding.
 *
 * If resolution fails permanently (a future build removes the method), the
 * bridge disables itself and says so in the log rather than injecting
 * plausible-looking wrong keys.
 */
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "nx_pointer.h"   /* nxp_cursor_visible */
#include "dfu_input_offsets.h"

extern so_module il2cpp_mod;

/* ---- resolved managed entry points --------------------------------------- */
typedef void  (*fn_set_key)(int32_t keycode, int32_t value);
typedef void  (*fn_set_axis)(int32_t action, float value);
typedef uint8_t (*fn_get_enabled)(void);
typedef void  (*fn_set_enabled)(int32_t value);
typedef void *(*fn_get_instance)(void);
typedef int32_t (*fn_get_default_bind)(void *thiz, int32_t action);
typedef void  (*fn_trigger_action)(int32_t action);

static fn_set_key          g_set_key;
static fn_set_axis         g_set_axis;
static fn_set_enabled      g_set_enabled;
static fn_get_instance     g_get_instance;
static fn_get_default_bind g_get_default_bind;
static fn_trigger_action   g_trigger_action;

static PadState g_pad;        /* initialised once in dfu_input_init   */
static int  g_ready;          /* offsets guard-checked and resolved  */
static int  g_disabled;       /* hard-off after an unrecoverable miss */
static int  g_binds_resolved; /* GetDefaultBinding cache populated    */

/* action -> KeyCode, filled from the game at runtime. 0 = unresolved. */
static int32_t g_bind[DFU_ActionUnknown];

/* Last value key() sent to the touchscreen layer, so it can send only changes.
 * FILE scope, not function-static, because dfu_input_release_all() calls
 * g_set_key() directly and must invalidate this -- otherwise the cache would
 * still say "held" after a release, and the next real press would be swallowed
 * as a no-change. That would have made buttons stop working after every
 * keyboard popup or cursor toggle. */
static uint8_t g_key_last[DFU_ActionUnknown];

/* ===================== KEYCODE STATE THE GAME READS ======================
 *
 * The pad writes here; UnityEngine.Input.GetKey / GetKeyDown / GetKeyUp are
 * hooked (main.c) and ANSWER from here. That inversion is the point: the old
 * path called SetKey INTO managed code from a C frame, where a managed
 * exception is std::terminate. Here managed code calls out to us and we return
 * a bool, so nothing can throw across the boundary.
 *
 * It is also the path Daggerfall's own InputManager already reads, so every
 * keybinding, menu and remap screen works untouched -- including the ones the
 * pad bridge never knew about.
 *
 * UnityEngine.KeyCode runs to 509 (Joystick8Button19); 512 covers it. */
#define DFU_KC_MAX 512
static uint8_t g_kc_now[DFU_KC_MAX];
static uint8_t g_kc_prev[DFU_KC_MAX];

static void kc_set(int32_t kc, int down) {
  if (kc > 0 && kc < DFU_KC_MAX) g_kc_now[kc] = down ? 1 : 0;
}
/* Called by the hooks. Unknown keycodes read as up, which on a Switch is not an
 * approximation but the truth: there is no keyboard, so nothing else can be
 * holding a key. That is why these hooks need no trampoline to the original. */
int dfu_kc_is_down(int32_t kc){ return (kc>0 && kc<DFU_KC_MAX) ? g_kc_now[kc] : 0; }
int dfu_kc_went_down(int32_t kc){ return (kc>0 && kc<DFU_KC_MAX) ? (g_kc_now[kc] && !g_kc_prev[kc]) : 0; }
int dfu_kc_went_up(int32_t kc){ return (kc>0 && kc<DFU_KC_MAX) ? (!g_kc_now[kc] && g_kc_prev[kc]) : 0; }
/* Latch at end of frame so GetKeyDown/GetKeyUp see exactly one frame of edge. */
void dfu_kc_frame_end(void){ memcpy(g_kc_prev, g_kc_now, sizeof g_kc_prev); }


/* Actions we actually drive from the pad. Everything else stays available
 * through the on-screen controls, which is the point of keeping Tier 1 live. */
/* HELD actions only. These are the ones that must stay down while a button is
 * down, so they go through SetKey and therefore need a KeyCode binding.
 * Everything one-shot goes through TriggerAction and needs no binding at all,
 * which is why most of the pad keeps working even if resolution never
 * succeeds. */
static const uint8_t k_held[] = {
  DFU_Jump, DFU_Crouch, DFU_Run, DFU_Sneak, DFU_ActivateCenterObject,
  DFU_ReadyWeapon, DFU_SwingWeapon, DFU_CastSpell, DFU_CenterView,
  /* MOVEMENT. These were missing, so g_bind[] stayed 0 for them and the stick
   * drove nothing but the axis API -- which Daggerfall's InputManager does not
   * read for walking. Its defaults are W/A/S/D, i.e. KeyCodes, so movement has
   * to come through a bound key like every other held action. */
  DFU_MoveForwards, DFU_MoveBackwards, DFU_MoveLeft, DFU_MoveRight,
  DFU_TurnLeft, DFU_TurnRight,
};

/* Drop EVERYTHING we are holding down.
 *
 * The pump is the only thing that ever clears g_kc_now, so any path that stops
 * running it leaves whatever was held at that instant held forever -- and the
 * Input.GetKey hooks keep reporting it to the game. That is "I can't stop
 * moving forward even though I'm not touching anything": the applet blocks the
 * render thread mid-stride, or the pump early-returns, and MoveForwards is
 * still down.
 *
 * Clearing the table is not enough on its own: the touchscreen layer was told
 * separately via SetKey/SetAxis and has to be told to let go too. */
void dfu_input_release_all(void) {
  int had = 0;
  for (int i = 0; i < DFU_KC_MAX; i++) if (g_kc_now[i]) { g_kc_now[i] = 0; had = 1; }
  /* Nothing held: do NOT spend ~15 managed SetKey calls and 4 SetAxis calls
   * saying so. This is reachable every frame, and each of those is a call into
   * IL2CPP from a C frame -- cheap individually, not free, and not worth
   * repeating forever once everything is already released. */
  if (!had) { dfu_kc_frame_end(); return; }
  if (g_set_key && g_binds_resolved)
    for (unsigned i = 0; i < sizeof k_held / sizeof k_held[0]; i++) {
      int32_t kc = g_bind[k_held[i]];
      if (kc) g_set_key(kc, 0);
    }
  memset(g_key_last, 0, sizeof g_key_last);   /* see g_key_last: must not go stale */
  if (g_set_axis) {
    g_set_axis(DFU_AXIS_MovementHorizontal, 0.0f);
    g_set_axis(DFU_AXIS_MovementVertical,   0.0f);
    g_set_axis(DFU_AXIS_CameraHorizontal,   0.0f);
    g_set_axis(DFU_AXIS_CameraVertical,     0.0f);
  }
  dfu_kc_frame_end();          /* prev := now(=0), so no stale GetKeyUp edge */
  if (had) debugPrintf("[input] released all held inputs\n");
}

static int guard_ok(const char *what, uint32_t rva, uint32_t want) {
  uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  uint32_t have = *(volatile uint32_t *)(b + rva);
  if (have == want) return 1;
  debugPrintf("[input] !! %s @il2cpp+0x%x guard MISMATCH (have 0x%08x, "
              "want 0x%08x)\n", what, rva, have, want);
  return 0;
}

/* Resolve and guard-check every entry point. Called once, after il2cpp is up.
 * A single mismatch disables the whole bridge: a wrong address here is a jump
 * into arbitrary code, not a silent bad write, so there is no safe partial
 * mode. Tier 1 (touch) is unaffected and the game stays playable. */
void dfu_input_init(void) {
  if (g_ready || g_disabled) return;
  uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
  if (!b) return;

  int ok = 1;
  ok &= guard_ok("TouchscreenInputManager.SetKey",
                 OFF_TSIM_SetKey, GUARD_TSIM_SetKey);
  ok &= guard_ok("TouchscreenInputManager.SetAxis",
                 OFF_TSIM_SetAxis, GUARD_TSIM_SetAxis);
  ok &= guard_ok("TouchscreenInputManager.set_IsTouchscreenInputEnabled",
                 OFF_TSIM_set_InputEnabled, GUARD_TSIM_set_InputEnabled);
  ok &= guard_ok("TouchscreenInputManager.TriggerAction",
                 OFF_TSIM_TriggerAction, GUARD_TSIM_TriggerAction);
  ok &= guard_ok("InputManager.get_Instance",
                 OFF_InputManager_get_Instance, GUARD_IM_get_Instance);
  ok &= guard_ok("InputManager.GetDefaultBinding",
                 OFF_InputManager_GetDefaultBind, GUARD_IM_GetDefaultBind);
  if (!ok) {
    g_disabled = 1;
    debugPrintf("[input] pad injection DISABLED (offset mismatch). The game's "
                "on-screen touch controls still work; run "
                "tools/verify_offsets.py against your libil2cpp.so.\n");
    return;
  }

  g_set_key          = (fn_set_key)(b + OFF_TSIM_SetKey);
  g_set_axis         = (fn_set_axis)(b + OFF_TSIM_SetAxis);
  g_set_enabled      = (fn_set_enabled)(b + OFF_TSIM_set_InputEnabled);
  g_trigger_action   = (fn_trigger_action)(b + OFF_TSIM_TriggerAction);
  g_get_instance     = (fn_get_instance)(b + OFF_InputManager_get_Instance);
  g_get_default_bind = (fn_get_default_bind)(b + OFF_InputManager_GetDefaultBind);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);

  g_ready = 1;
  debugPrintf("[input] pad injection armed (SetKey @0x%x, SetAxis @0x%x)\n",
              OFF_TSIM_SetKey, OFF_TSIM_SetAxis);
}

/* Ask the game for its own bindings. Cheap and idempotent; retried each frame
 * until InputManager exists, because it is created during scene load and the
 * first frames run before it. */
static void resolve_binds(void) {
  if (g_binds_resolved) return;
  void *im = g_get_instance();
  if (!im) return;                     /* not up yet -- try again next frame */
  for (unsigned i = 0; i < sizeof k_held; i++) {
    int a = k_held[i];
    g_bind[a] = g_get_default_bind(im, a);
  }
  g_binds_resolved = 1;
  debugPrintf("[input] resolved %u held-action bindings from InputManager "
              "(e.g. Jump->KeyCode %d, Run->KeyCode %d)\n",
              (unsigned)sizeof k_held, g_bind[DFU_Jump], g_bind[DFU_Run]);
}

static void key(int action, int down) {
  int32_t kc = g_bind[action];
  if (!kc) return;
  down = !!down;
  kc_set(kc, down);          /* what Input.GetKey* will report -- the real path */

  /* Only tell the touchscreen layer when the state actually CHANGES. This used
   * to fire every frame for every bound action -- around fifteen calls into
   * managed IL2CPP per frame, forever, just to repeat what it already knew.
   * Each one is a call from a C frame where an exception would be fatal, so
   * the cheapest thing to do with them is not make them. */
  if (action >= 0 && action < DFU_ActionUnknown) {
    if (g_key_last[action] == (uint8_t)down) return;
    g_key_last[action] = (uint8_t)down;
  }
  if (g_set_key) g_set_key(kc, down);
}

/* Deadzoned, normalised stick value. The Switch reports +-32767; Daggerfall's
 * axis actions want roughly -1..1 like a Unity axis. */
static float stick(int16_t v, int dead) {
  int a = v < 0 ? -v : v;
  if (a <= dead) return 0.0f;
  float n = (float)(a - dead) / (float)(32767 - dead);
  if (n > 1.0f) n = 1.0f;
  return v < 0 ? -n : n;
}

/* Called once per frame from the render loop, immediately before
 * nativeRender, on the thread Unity runs managed Update on. See the threading
 * note in dfu_input_offsets.h -- do not call this from anywhere else. */
void dfu_input_pump(void) {
  if (!g_ready || g_disabled) return;

  /* The virtual cursor steers with the LEFT STICK and taps with A, so those two
   * clash with the pad bridge and nothing else does.
   *
   * The first version of this fix returned early and disabled the whole bridge
   * while the cursor was up. That is wrong when DOCKED: nx_pointer forces the
   * cursor on there (no touchscreen, so there must always be a way to point),
   * which would have left a docked player with no buttons at all -- worse than
   * the phantom input it was fixing. Suppress the two that conflict, keep the
   * rest. */
  const int cursor_owns = nxp_cursor_visible();

  /* DO NOT enter managed code before IL2CPP is running it itself. g_ready only
   * means the offsets matched; it says nothing about whether the runtime is
   * initialised. SetAxis/SetKey/TriggerAction are managed IL2CPP methods whose
   * prologues touch class statics, and on frame 1 those are NULL.
   *
   * This is the same fault that killed the first boot through a different door:
   * nx_time_tick() called an icall resolver stub too early and the runtime
   * crashed building the exception it meant to throw (AUDIT.md sec 15). That
   * one was found the hard way; this one was found by looking for siblings.
   *
   * port_managed_live() is set by the Time.get_* hooks, which only managed code
   * can reach -- so it is true exactly when entering managed code is safe. */
  extern int port_managed_live(void);
  if (!port_managed_live()) { dfu_input_release_all(); return; }
  { static int announced = 0;
    if (!announced) { announced = 1;
      debugPrintf("[input] managed side live -- pad bridge active\n"); } }
  resolve_binds();

  /* Latch the previous frame's state HERE, before this frame's keys are set.
   * It used to be latched at the END of the pump, in the same call that set
   * them -- so by the time the game read Input.GetKeyDown, prev already equalled
   * now and the edge had been erased. GetKeyDown could never fire, and
   * GetKeyUp never fired either. Latching first leaves the edge visible for the
   * whole frame, which is what the game samples. */
  dfu_kc_frame_end();

  padUpdate(&g_pad);
  uint64_t held = padGetButtons(&g_pad);
  HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
  HidAnalogStickState rs = padGetStickPos(&g_pad, 1);

  /* --- axes: no binding needed, these are action-level ------------------- */
  g_set_axis(DFU_AXIS_MovementHorizontal, cursor_owns ? 0.0f : stick(ls.x, 3000));
  g_set_axis(DFU_AXIS_MovementVertical,   cursor_owns ? 0.0f : stick(ls.y, 3000));

  /* Left stick and D-pad also drive the bound MOVEMENT KEYS. Daggerfall's
   * InputManager reads movement as keys (its defaults are W/A/S/D), so the axis
   * API alone left the character standing still -- which is most of "the sticks
   * don't work". Threshold is well past the axis deadzone so a resting stick
   * never holds a key down. */
  {
    const int T = 9000;
    /* Only the stick is taken away -- it is the one the cursor steers with.
     * The D-pad keeps walking. It also scrolls the cursor (nx_pointer's
     * do_dpad is not gated on visibility), but that overlap predates this
     * change and has not been reported as a problem; noting it rather than
     * silently widening the fix. */
    const int sx = cursor_owns ? 0 : ls.x, sy = cursor_owns ? 0 : ls.y;
    int up    = (sy >  T) || (held & HidNpadButton_Up);
    int down_ = (sy < -T) || (held & HidNpadButton_Down);
    int left  = (sx < -T) || (held & HidNpadButton_Left);
    int right = (sx >  T) || (held & HidNpadButton_Right);
    key(DFU_MoveForwards, up);   key(DFU_MoveBackwards, down_);
    key(DFU_MoveLeft,     left); key(DFU_MoveRight,     right);
  }
  g_set_axis(DFU_AXIS_CameraHorizontal,   stick(rs.x, 2500));
  g_set_axis(DFU_AXIS_CameraVertical,     stick(rs.y, 2500));

  /* --- buttons ----------------------------------------------------------- */
  /* ZL is a shift: Daggerfall has 45 actions and a pad has ~16 buttons.
   * Nothing is lost by the omissions -- every action stays reachable through
   * the game's on-screen controls, which is why Tier 1 stays live. */
  static uint64_t prev;
  uint64_t pressed = held & ~prev;          /* rising edges only */
  prev = held;

  int mod = !!(held & HidNpadButton_ZL);

  /* One-shot actions: TriggerAction takes an Action directly, so these work
   * with no binding resolution. Edge-triggered -- TriggerAction runs a
   * press/release coroutine, so re-firing it every frame while a button is
   * held would retrigger it continuously. */
  if (!mod) {
    if (pressed & HidNpadButton_Up)    g_trigger_action(DFU_Inventory);
    if (pressed & HidNpadButton_Down)  g_trigger_action(DFU_CharacterSheet);
    if (pressed & HidNpadButton_Left)  g_trigger_action(DFU_TravelMap);
    if (pressed & HidNpadButton_Right) g_trigger_action(DFU_AutoMap);
    if (pressed & HidNpadButton_Plus)  g_trigger_action(DFU_Escape);
    if (pressed & HidNpadButton_Minus) g_trigger_action(DFU_Status);
  } else {
    if (pressed & HidNpadButton_B)      g_trigger_action(DFU_Rest);
    if (pressed & HidNpadButton_A)      g_trigger_action(DFU_Transport);
    if (pressed & HidNpadButton_X)      g_trigger_action(DFU_UseMagicItem);
    if (pressed & HidNpadButton_Y)      g_trigger_action(DFU_SwitchHand);
    if (pressed & HidNpadButton_Up)     g_trigger_action(DFU_StealMode);
    if (pressed & HidNpadButton_Down)   g_trigger_action(DFU_GrabMode);
    if (pressed & HidNpadButton_Left)   g_trigger_action(DFU_InfoMode);
    if (pressed & HidNpadButton_Right)  g_trigger_action(DFU_TalkMode);
    if (pressed & HidNpadButton_L)      g_trigger_action(DFU_LogBook);
    if (pressed & HidNpadButton_R)      g_trigger_action(DFU_NoteBook);
    if (pressed & HidNpadButton_Plus)   g_trigger_action(DFU_QuickSave);
    if (pressed & HidNpadButton_Minus)  g_trigger_action(DFU_QuickLoad);
    if (pressed & HidNpadButton_StickR) g_trigger_action(DFU_ToggleConsole);
  }

  /* Held actions need the KeyCode binding. If it has not resolved yet we skip
   * these rather than inject a wrong key; axes and one-shots still work. */
  if (!g_binds_resolved) { dfu_kc_frame_end(); return; }

  int play = !mod;   /* held actions are unmodified-only */
  key(DFU_Jump,                 play && (held & HidNpadButton_B));
  /* A taps the cursor while it is visible; letting it also activate would fire
   * both on one press, which is the "A keeps being pressed in the menu" half. */
  key(DFU_ActivateCenterObject, play && !cursor_owns && (held & HidNpadButton_A));
  key(DFU_CastSpell,            play && (held & HidNpadButton_X));
  key(DFU_Crouch,               play && (held & HidNpadButton_Y));
  key(DFU_Sneak,                play && (held & HidNpadButton_L));
  key(DFU_ReadyWeapon,          play && (held & HidNpadButton_R));
  key(DFU_SwingWeapon,          play && (held & HidNpadButton_ZR));
  key(DFU_Run,                  play && (held & HidNpadButton_StickL));
  key(DFU_CenterView,           play && (held & HidNpadButton_StickR));

  /* NOT latched here -- see the note at the top of this function. */
}
