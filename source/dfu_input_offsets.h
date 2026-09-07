/* dfu_input_offsets.h -- Daggerfall Unity managed input entry points.
 *
 * Target: libil2cpp.so, BuildID sha1 cc72326dbdd256d9043b0690a3da85eea77f0cda.
 * RVAs are read from dump.cs; every one is guard-checked at runtime before use
 * (see dfu_input.c), because calling a wrong address is a jump into arbitrary
 * code rather than a silent corruption.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE EXISTS -- a correction to the port's earlier assumption
 * ---------------------------------------------------------------------------
 * The first pass through this port assumed Daggerfall Unity is a
 * keyboard-and-mouse game and that input would mean building an Android
 * KeyEvent path plus a bind scheme, and that it was the largest job left.
 *
 * That was wrong, and it was wrong because it reasoned from desktop Daggerfall
 * Unity instead of from the binary in front of it. THIS build is the Android
 * one, and it ships a complete touchscreen input layer:
 *
 *     TouchscreenInputManager          on-screen controls + action injection
 *     VirtualJoystick                  movement/look sticks
 *     TouchscreenButton                per-button {Actions myAction; KeyCode myKey;}
 *     TouchscreenButtonConfiguration   saved layouts
 *     TouchscreenLayoutConfiguration
 *     DaggerfallJoystickControlsWindow in-game control config UI
 *
 * and TouchscreenInputManager keeps
 *     private static Dictionary<int,float> axes;
 *     private static Dictionary<int,bool>  keys;
 * feeding InputManager's 45 Actions and 4 AxisActions DIRECTLY, bypassing
 * KeyCode resolution entirely for axes.
 *
 * Two consequences, both good:
 *
 *   1. The game's own on-screen controls work through the ordinary touch path
 *      the PvZ base already implements. In handheld, with a real touchscreen,
 *      that is a playable baseline needing NO new code.
 *
 *   2. For docked play and physical sticks, the pad can be mapped onto the
 *      game's action system by calling two public static methods. No synthetic
 *      MotionEvent, no KeyEvent plumbing, no bind scheme to invent.
 *
 * ---------------------------------------------------------------------------
 * CALLING CONVENTION -- verified by disassembly, not assumed
 * ---------------------------------------------------------------------------
 * IL2CPP normally appends a MethodInfo* to every managed method's argument
 * list, and passing NULL where it is dereferenced is a crash. Both injection
 * methods were disassembled to check:
 *
 *   SetKey(KeyCode k, bool value)          @0x1E02FFC
 *     0x1e02ffc  str  x30,[sp,#-0x30]!
 *     0x1e03008  adrp x22,#0x45b3000
 *     0x1e03010  ldrb w8,[x22,#0x533]      cctor-finished flag
 *     0x1e03018  mov  w19,w1               <- arg1 = value   (bool)
 *     0x1e0301c  mov  w20,w0               <- arg0 = keycode (int32)
 *     0x1e03020  tbnz w8,#0,...            skip class init if already done
 *
 *   SetAxis(AxisActions a, float value)    @0x1E02E80
 *     0x1e02e80  str  d8,[sp,#-0x30]!
 *     0x1e02e94  ldrb w8,[x21,#0x531]      cctor-finished flag
 *     0x1e02e9c  mov  v8.16b,v0.16b        <- arg1 = value   (float, in s0/v0)
 *     0x1e02ea0  mov  w19,w0               <- arg0 = action  (int32)
 *
 * Neither reads x1/x2 as a MethodInfo -- SetKey uses w1 as its bool argument --
 * so the MethodInfo slot is unused and may be omitted. Both also perform their
 * own class-initialisation check, so they are safe to call before any managed
 * code has touched TouchscreenInputManager.
 *
 * ---------------------------------------------------------------------------
 * WHICH INJECTION POINT, AND THE AUDIT THAT SETTLED IT
 * ---------------------------------------------------------------------------
 * dump.cs lists declarations only -- every body is `{ }` -- so grepping it for
 * a type name finds fields and parameters and NEVER call sites. It cannot tell
 * you whether injected state is actually consumed. That question was settled by
 * scanning libil2cpp .text (tools/xcall.py, tools/xdata.py):
 *
 *   SetAxis  -> CONSUMED. TouchscreenInputManager.GetAxis has 4 direct call
 *               sites, in InputManager::Update() and
 *               InputManager::FindInputAxisActions(). Axis injection is real.
 *
 *   SetKey   -> CONSUMED, but indirectly. GetKey has ZERO direct call sites,
 *               which initially looked like a dead path. It is not: il2cpp
 *               emits C++ and the C++ compiler inlined it. Cross-referencing
 *               the TouchscreenInputManager class pointer (0x42cb560) instead
 *               of the method shows 246 reference sites, including
 *               InputManager::GetPollKey(), InputManager::Update() and
 *               InputManager::UpdateLook(). SetKey's write reaches the game.
 *
 * That cross-reference also turned up a BETTER entry point for one-shot input:
 *
 *   TriggerAction(InputManager.Actions)  @0x1E031B0
 *       Takes an ACTION directly, so it needs no KeyCode binding at all. It
 *       starts <TriggerActionCoroutine>d__94, i.e. a press-and-release over a
 *       frame or two -- correct for menus and mode toggles, WRONG for anything
 *       held (you cannot hold Run with a one-shot).
 *
 * So this port uses both, split by semantics rather than convenience:
 *       held actions (Run, Sneak, Jump, weapon, cast)  -> SetKey + binding
 *       one-shot actions (menus, mode toggles, saves)  -> TriggerAction
 * which also means most buttons work even if binding resolution never succeeds.
 *
 * THREADING, which is the real risk: these are managed methods. They must be
 * called from a thread il2cpp has attached, or thread-local GC/domain lookups
 * fault. dfu_input.c therefore injects only from the render-loop thread and
 * only immediately before nativeRender, which is the thread Unity is already
 * running managed Update on. Do not call these from the pointer or audio
 * threads.
 */
#ifndef DFU_INPUT_OFFSETS_H
#define DFU_INPUT_OFFSETS_H

#include <stdint.h>

/* ---- libil2cpp RVAs (link-time; runtime = il2cpp virtbase + RVA) --------- */
#define OFF_TSIM_SetKey                 0x1E02FFC  /* (KeyCode,bool)          */
#define OFF_TSIM_SetAxis                0x1E02E80  /* (AxisActions,float)     */
#define OFF_TSIM_get_InputEnabled       0x1E02BAC  /* ()->bool                */
#define OFF_TSIM_set_InputEnabled       0x1E02C00  /* (bool)                  */
#define OFF_InputManager_get_Instance   0x1E4F33C  /* ()->InputManager        */
#define OFF_InputManager_GetDefaultBind 0x1E58038  /* (this,Actions)->KeyCode */
#define OFF_TSIM_TriggerAction          0x1E031B0  /* (Actions) one-shot      */

/* First instruction at each, read from the binary. Checked before any call. */
#define GUARD_TSIM_SetKey               0xF81D0FFEu /* str x30,[sp,#-0x30]!   */
#define GUARD_TSIM_SetAxis              0xFC1D0FE8u /* str d8,[sp,#-0x30]!    */
#define GUARD_TSIM_get_InputEnabled     0xF81E0FFEu /* str x30,[sp,#-0x20]!   */
#define GUARD_TSIM_set_InputEnabled     0xA9BE57FEu /* stp x30,x21,[sp,..]!   */
#define GUARD_IM_get_Instance           0xF81E0FFEu /* str x30,[sp,#-0x20]!   */
#define GUARD_IM_GetDefaultBind         0xD100C3FFu /* sub sp,sp,#0x30        */
#define GUARD_TSIM_TriggerAction        0xA9BE57FEu /* stp x30,x21,[sp,#-0x20]! */

/* ---- InputManager.AxisActions ------------------------------------------- */
typedef enum {
  DFU_AXIS_MovementHorizontal = 0,
  DFU_AXIS_MovementVertical   = 1,
  DFU_AXIS_CameraHorizontal   = 2,
  DFU_AXIS_CameraVertical     = 3,
} DfuAxisAction;

/* ---- InputManager.Actions (45 + Unknown), read from dump.cs -------------- */
typedef enum {
  DFU_Escape = 0,          DFU_ToggleConsole = 1,   DFU_MoveForwards = 2,
  DFU_MoveBackwards = 3,   DFU_TurnLeft = 4,        DFU_MoveLeft = 5,
  DFU_TurnRight = 6,       DFU_MoveRight = 7,       DFU_FloatUp = 8,
  DFU_FloatDown = 9,       DFU_Jump = 10,           DFU_Crouch = 11,
  DFU_Slide = 12,          DFU_Run = 13,            DFU_Rest = 14,
  DFU_Transport = 15,      DFU_StealMode = 16,      DFU_GrabMode = 17,
  DFU_InfoMode = 18,       DFU_TalkMode = 19,       DFU_CastSpell = 20,
  DFU_RecastSpell = 21,    DFU_AbortSpell = 22,     DFU_UseMagicItem = 23,
  DFU_ReadyWeapon = 24,    DFU_SwingWeapon = 25,    DFU_SwitchHand = 26,
  DFU_Status = 27,         DFU_CharacterSheet = 28, DFU_Inventory = 29,
  DFU_ActivateCenterObject = 30, DFU_ActivateCursor = 31,
  DFU_LookUp = 32,         DFU_LookDown = 33,       DFU_CenterView = 34,
  DFU_Sneak = 35,          DFU_LogBook = 36,        DFU_NoteBook = 37,
  DFU_AutoMap = 38,        DFU_TravelMap = 39,      DFU_QuickSave = 40,
  DFU_QuickLoad = 41,      DFU_PrintScreen = 42,    DFU_AutoRun = 43,
  DFU_ToggleRun = 44,      DFU_ActionUnknown = 45,
} DfuAction;

#endif /* DFU_INPUT_OFFSETS_H */
