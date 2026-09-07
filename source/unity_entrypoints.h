/* unity_entrypoints.h -- UnityPlayer native methods recovered from
 * libunity.so's JNI_OnLoad  (DAGGERFALL UNITY, Unity 2022.3.62f3, arm64 /
 * IL2CPP).
 *
 * Target binary: libunity.so  BuildID xxHash 1dc173bbf7a97f88.
 * If your copy differs, re-run:  python3 tools/extract_entrypoints.py libunity.so
 *
 * These were auto-extracted from THIS build by parsing JNINativeMethod triples
 * ({const char *name; const char *sig; void *fn;}, 24 bytes each on arm64) out
 * of the R_AARCH64_RELATIVE addends in .rela.dyn -- NOT by shifting the PvZ
 * table. In a PIE .so all three fields are relocations, so the file bytes are
 * zero and the real values exist only in the addends.
 *
 * The extractor found 53 JNINativeMethod entries across 10 tables. The main
 * table at 0x1127e48 holds 29: the 26 UnityPlayer natives below, plus three
 * Choreographer/Swappy natives listed at the bottom as FYI. The method set is
 * IDENTICAL to PvZ Fusion 3.8.1 and Zookeeper DX -- same 2022.3.62 engine --
 * so the drive code in main.c needs no change beyond these offsets.
 *
 * Runtime address = unity_mod.load_virtbase + offset (the .so links at base 0).
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- UnityPlayer native method offsets (link-time vaddr) ---------------- */
/* JNI_OnLoad -- exported symbol, confirmed against .dynsym */
#define OFF_JNI_OnLoad                    0x6738e4 /* (JavaVM*,reserved)->jint */

/* drive-critical */
#define OFF_initJni                       0x672ae0 /* (Landroid/content/Context;)V        */
#define OFF_nativeRecreateGfxState        0x672d14 /* (ILandroid/view/Surface;)V  surface */
#define OFF_nativeSendSurfaceChangedEvent 0x672d7c /* ()V                                 */
#define OFF_nativeRender                  0x672dd4 /* ()Z   per-frame; false=stop         */
#define OFF_nativeInjectEvent             0x672e34 /* (Landroid/view/InputEvent;I)Z       */
#define OFF_nativePause                   0x672b7c /* ()Z                                 */
#define OFF_nativeResume                  0x672be0 /* ()V                                 */
#define OFF_nativeFocusChanged            0x672cc0 /* (Z)V                                */
#define OFF_nativeDone                    0x672aec /* ()Z   shutdown                      */
#define OFF_nativeApplicationUnload       0x672c70 /* ()V                                 */
#define OFF_nativeLowMemory               0x672c28 /* ()V                                 */
#define OFF_nativeOrientationChanged      0x67382c /* (II)V                               */

/* secondary / usually unused for a port */
#define OFF_nativeUnitySendMessage        0x673440 /* (String,String,[B)V                 */
#define OFF_nativeMuteMasterAudio         0x673650 /* (Z)V                                */
#define OFF_nativeGetNoWindowMode         0x67388c /* ()Z                                 */
#define OFF_nativeIsAutorotationOn        0x6735f0 /* ()Z                                 */
#define OFF_nativeSetLaunchURL            0x6736ac /* (String)V                           */
#define OFF_nativeHidePreservedContent    0x6737e4 /* ()V                                 */

/* soft keyboard -- routed via the SoftInputProvider stub. NOT optional for
 * Daggerfall the way it was for PvZ: DFU asks for text input at character
 * creation (name entry) and when naming saves. See PORTING.md sec 3. */
#define OFF_nativeSetInputArea            0x673128 /* (IIII)V                             */
#define OFF_nativeSetKeyboardIsVisible    0x6731a8 /* (Z)V                                */
#define OFF_nativeSetInputString          0x673200 /* (String)V                           */
#define OFF_nativeSetInputSelection       0x6732a0 /* (II)V                               */
#define OFF_nativeSoftInputClosed         0x6733f0 /* ()V                                 */
#define OFF_nativeSoftInputCanceled       0x673308 /* ()V                                 */
#define OFF_nativeSoftInputLostFocus      0x673358 /* ()V                                 */
#define OFF_nativeReportKeyboardConfigChanged 0x6733a8 /* ()V                             */
#define OFF_nativeSendSurfaceChanged      OFF_nativeSendSurfaceChangedEvent

/* ---- JNI native signatures: ret (*)(JNIEnv*, jobject thiz, args...) ----- */
typedef void     (*fn_initJni)(void*,void*,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what the Java UnityPlayer does; main.c does it instead):
 *
 *   initJni(env, thiz, fake_context);
 *   nativeRecreateGfxState(env, thiz, 0, fake_surface);
 *   nativeSendSurfaceChangedEvent(env, thiz);
 *   for (;;) {
 *       // input: nativeInjectEvent(env,thiz, event, deviceId);
 *       if (!nativeRender(env, thiz)) break;
 *   }
 *   nativeApplicationUnload(env, thiz);  nativeDone(env, thiz);
 *
 * NOTE on input: nativeInjectEvent takes a Java InputEvent jobject which the
 * engine queries back via JNI. For a touch game that means MotionEvent getters.
 * Daggerfall is a KEYBOARD-AND-MOUSE game -- it needs the KeyEvent path
 * (getAction/getKeyCode/getUnicodeChar/getMetaState) too, and that path is
 * exercised far more lightly by the PvZ base. Largest piece of work left.
 * =========================================================================== */

/* ---- Non-UnityPlayer native tables also in this build (FYI, never driven) --
 *   choreographer   nOnChoreographer                      @0xbd68a0
 *   swappy          nOnRefreshPeriodChanged               @0xbd8ba0
 *                   nSetSupportedRefreshPeriods           @0xbd89c0
 *   ARCore          initializeARCore/pause/resume         @0x64c000/0x64c064/0x64c0b8
 *   Camera2         initCamera2Jni/deinit                 @0x66bc48/0x66bc94
 *                   nativeFrameReady/SurfaceTextureReady  @0x6702a0/0x670138
 *   HFP audio       initHFPStatusJni/deinit               @0x64f2ec/0x64f338
 *   audio volume    onAudioVolumeChanged                  @0x655620
 *   query status    nativeStatusQueryResult               @0x64e4ac
 *   orient lock     nativeUpdateOrientationLockState      @0x655864
 *   softinput type  nativeGetSoftInputType                @0x648bb0
 *   il2cpp proxy    nativeProxyInvoke/Finalize/...        @0x3b7130 ...
 *   FMOD java out   fmodGetInfo/fmodProcess/...           @0xdcf208 ...
 * -------------------------------------------------------------------------- */

#endif /* UNITY_ENTRYPOINTS_H */
