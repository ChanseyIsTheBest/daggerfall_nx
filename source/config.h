/* config.h -- config.txt next to the .nro, plus the hardcoded decisions.
 *
 * MIT licensed. See LICENSE.
 */
#ifndef PPS_CONFIG_H
#define PPS_CONFIG_H

void cfg_load(void);

const char *cfg_language(void);   /* "en", "de", ... or the console's own    */
const char *cfg_country(void);    /* "GB", "US", ...                          */
int   cfg_vsync(void);
float cfg_master_volume(void);
int   cfg_target_fps(void);       /* what the engine is told to aim for       */
int   cfg_purchases_enabled(void);

/* ------------------------------------------------------------------ */
/* Render size                                                         */
/* ------------------------------------------------------------------ */

/* 1080p in handheld as well as docked.
 *
 * The handheld panel is 720p, so the compositor downscales. That costs almost
 * nothing for a 2.5D casual title, and a fixed render size means the whole
 * class of surface-change bugs stops existing: the engine is told one size
 * once and never has to be told again.
 *
 * That matters more here than it did for Osmos, because this engine's
 * onSurfaceChanged path calls BOTH init(w,h,rot) AND updateScreenSize(w,h),
 * and init() is the constructor for the entire application object. Firing it
 * a second time on a dock would re-enter engine startup rather than resize
 * anything. With a fixed size it can only ever run once, which is the shape
 * the Java code assumed too -- GameRenderer guards init with mSurfaceCreated
 * and never expects a second call with different dimensions. */
/* MEASURED ON HARDWARE: the engine asks for 640x960 with rotation 1.
 *
 *     [I] GameLib.setupPlatform(640, 960, rot 1, 60 fps, msaa 0)
 *
 * That is PORTRAIT, 2:3, and rotation 1 is SCREEN_ROTATION_90. setupPlatform
 * is how native tells the platform layer what surface it wants, so this is the
 * engine's own answer to "what shape am I". Papa Pear Saga shipped portrait on
 * phones and the aspect it names is a phone aspect.
 *
 * We give it 1920x1080 anyway, and it accepts that -- init() and
 * updateScreenSize() both take the real size and it lays out from those. But
 * if the interface turns out to be positioned for a 2:3 screen, this is the
 * first thing to change, and main.c logs the mismatch every boot so the
 * evidence is in front of you rather than in a comment.
 *
 * Rendering portrait on a landscape panel means letterboxing to roughly
 * 720x1080 with bars either side. If you want to try it, set these to 720 and
 * 1080 and leave PPS_ROTATION at 0 -- do NOT set the rotation to 1 as well,
 * because that value only tells the engine how to compensate its input
 * transform, and a non-zero one rotates touch away from the display. */
#define PPS_RENDER_W 1920
#define PPS_RENDER_H 1080

/* The touch panel reports in 1280x720 regardless of render size, so touches
 * have to be scaled up into render space before they reach onTouchEvent. */
#define PPS_PANEL_W  1280
#define PPS_PANEL_H  720

/* GameRenderer$ScreenRotation.getId(): SCREEN_ROTATION_0 is ordinal 0 and the
 * console never rotates. The engine is landscape-only here.
 *
 * Do not be tempted to pass 90 or 270 to get the portrait layout: this game
 * shipped portrait on phones and landscape on tablets, and it selects between
 * them from the ASPECT of the surface, not from this value. A 16:9 landscape
 * surface already picks the landscape layout; the rotation id only tells the
 * engine how to compensate its input transform, so a non-zero value here
 * rotates the touch mapping away from the display. */
#define PPS_ROTATION 0

/* setupPlatform(width, height, rotation, targetFps, msaaSamples).
 *
 * MSAA 0: the engine composites through an offscreen FBO, and a multisampled
 * default framebuffer would be resolved and then discarded. Nothing on screen
 * comes from the window surface directly. */
#define PPS_MSAA_SAMPLES 0

/* Frame pacing. The Java renderer slept to hold getTargetFps(); here the EGL
 * swap interval does that job, so this is only what the engine is told. 60 is
 * what the Android build asked for on a capable device. */
#define PPS_TARGET_FPS_DEFAULT 60

/* Device metrics handed to Device.getDpi(). The engine divides by 160 to get a
 * density bucket and picks its asset scale from it; 320 is the xhdpi bucket,
 * which is the tier whose art this APK actually ships. */
#define PPS_DPI 320.0f

/* Device.isTablet(). True: at 16:9 and 1920 wide this is a tablet-class
 * surface, and the phone layout would letterbox its HUD into the middle. */
#define PPS_IS_TABLET 1

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

/* Engine touch types, decoded from GameView.onTouchEvent's packed-switch:
 * Android's ACTION_* are mapped down to these four before they reach native.
 * ACTION_POINTER_DOWN/UP fold into DOWN/UP and ACTION_OUTSIDE into MOVE. */
#define PPS_TOUCH_DOWN   0
#define PPS_TOUCH_UP     1
#define PPS_TOUCH_MOVE   2
#define PPS_TOUCH_CANCEL 3

/* How many pointers the engine is fed. The panel reports up to 16; the game
 * is a one-finger aim-and-fire and its own code only ever tracks a couple. */
#define PPS_MAX_TOUCHES 4

/* ------------------------------------------------------------------ */
/* System events                                                       */
/* ------------------------------------------------------------------ */

/* NativeApplication$ESystemEvent ordinals, in declaration order from the
 * enum's <clinit>. These are ordinals, not opaque ids: the Java side passed
 * ESystemEvent.ordinal() straight through to onSystemEvent(int). */
#define PPS_EV_WILL_RESIGN_ACTIVE   0
#define PPS_EV_DID_BECOME_ACTIVE    1
#define PPS_EV_WILL_TERMINATE       2
#define PPS_EV_DID_ENTER_BACKGROUND 3
#define PPS_EV_WILL_ENTER_FOREGROUND 4
#define PPS_EV_GL_CONTEXT_RECREATED 5
#define PPS_EV_MEMORY_WARNING       6

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* ON, because this port has never run on hardware.
 *
 * With this at 0 a normal session writes no debug.log at all, which is the
 * right default once the port works. It is the wrong default now: the failures
 * ahead -- a static initialiser faulting, a shader that will not compile, an
 * asset root one directory off -- all present identically as "it stopped", and
 * the boot log plus diag_phase is the difference between knowing where and
 * guessing.
 *
 * Set it back to 0 once you are past bring-up. Warnings and errors are logged
 * either way; they open the file lazily so a clean run leaves nothing behind.
 *
 * PPS_DIAG stays 0. That one is the heavy tracing -- a line per JNI call and
 * per touch -- and at 60 fps it writes faster than the SD card will take it,
 * which changes the timing of whatever you were trying to observe. Turn it on
 * only for a specific question, and expect the frame rate to suffer. */
#ifndef DEBUG_LOG
#define DEBUG_LOG 1
#endif

#ifndef PPS_DIAG
#define PPS_DIAG 0
#endif

/* Mirror the engine's own __android_log_print output into debug.log. Cheap
 * (this engine logs perhaps a hundred lines a boot, not per frame) and it is
 * the single most useful thing to have when the screen is black, because the
 * King engine reports its own asset and shader failures through it. Follows
 * DEBUG_LOG so it costs nothing in a normal build. */
#define PPS_MIRROR_ENGINE_LOG DEBUG_LOG

/* ------------------------------------------------------------------ */
/* compatibility with the reused files                                 */
/* ------------------------------------------------------------------ */

/* opensles.c is compiled in unmodified from the Sonic Jump port, and that port
 * kept its settings in a `Config config` global rather than behind accessors.
 * It reads exactly one field. Providing it here is what lets opensles.c drop
 * in untouched, which is the same trade the forwarder headers make for
 * libc_shim.c -- upstream fixes stay pullable.
 *
 * decode_stream_audio gates OpenSL's MIME / Android-fd source path, which
 * decodes a compressed stream rather than taking PCM from a buffer queue.
 * This engine feeds the buffer queue -- it carries its own Vorbis decoder --
 * so the path is unlikely to be reached. It is left enabled because the cost
 * of being wrong in that direction is a log line, and the cost of being wrong
 * in the other is silence with no explanation. */
typedef struct { int decode_stream_audio; } PpsCompatConfig;
extern PpsCompatConfig config;

void log_init(void);
void log_close(void);
void log_write(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void overlay_note(const char *text);
/* error_screen() and fatal_error() are declared in error.h. */

#define LOGW(...) log_write('W', __VA_ARGS__)
#define LOGE(...) log_write('E', __VA_ARGS__)

#if DEBUG_LOG
#define LOGB(...) log_write('I', __VA_ARGS__)
#else
#define LOGB(...) ((void)0)
#endif

#if DEBUG_LOG && PPS_DIAG
#define LOGI(...) log_write('I', __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif

#endif
