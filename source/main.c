/* main.c -- papapear_nx entry point and frame loop.
 *
 * MIT licensed. See LICENSE. Ships no game code and no game assets.
 *
 * WHAT SHAPE OF PORT THIS IS
 * --------------------------
 * Papa Pear Saga is a GLSurfaceView + JNI game, not a NativeActivity one. The
 * Java side owned the loop and called instance natives on
 * com.king.core.NativeApplication, so *we* drive the engine here rather than
 * impersonating Android around it. That is why this file has a visible frame
 * loop and there is no android_native.c in the tree.
 *
 * Verified against the library:
 *   - it imports NO ANativeActivity, ALooper, AInputQueue or ANativeWindow
 *     symbol; the only libandroid.so imports are the asset manager and
 *     AConfiguration
 *   - it imports exactly one egl* symbol, eglGetProcAddress. GLSurfaceView
 *     owned the context on Android, so this file owns it here and the engine
 *     never sees it
 *   - its C++ runtime is statically linked inside it, so there is one module
 *     to load and no libc++_shared.so
 *
 * THE BRING-UP ORDER IS MEASURED, NOT GUESSED
 * -------------------------------------------
 * This is the part that usually costs the first hardware session. It did not
 * have to here: the order below was read out of the Dalvik bytecode of
 * GameActivity.onCreate and GameRenderer.onSurfaceChanged/onDrawFrame, so it
 * is what the Java layer actually did rather than what seemed reasonable.
 *
 *     PlatformProxy.createNativeInstance(activity)          -> jlong
 *     new NativeApplication()
 *     NativeApplication.getCrashReportIfAvailable()
 *     NativeApplication.create(thatLong, activity, context, assetManager)
 *       -- GL surface ready --
 *     NativeApplication.init(width, height, rotation)
 *     NativeApplication.updateScreenSize(width, height)
 *       -- per frame --
 *     NativeApplication.updateOrientation(rot)   only when rotation CHANGES
 *     NativeApplication.step(dtSeconds)          -> false means quit
 *
 * Three details from that reading that are easy to get wrong and silent when
 * you do:
 *
 *   1. step() takes SECONDS as a float. The Java code computed
 *      (nanoTime() - last) / 1.0e9f -- the constant is 0x4e6e6b28 in the
 *      bytecode, which is 1e9 as a float. Passing milliseconds runs the game
 *      a thousand times fast, which looks like a hang rather than a speed bug.
 *
 *   2. step() returning FALSE is the quit signal, not true. The Java renderer
 *      did `if (result == 0) { setRenderMode(0); requestApplicationMinimization(); }`
 *      so an inverted test exits on the first frame.
 *
 *   3. The very first onDrawFrame returned early WITHOUT stepping, guarded by
 *      mFirstDrawFrame. Reproduced below. The engine finishes its GL setup on
 *      the render thread during that frame and this costs one frame to
 *      respect.
 */

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <malloc.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"
#include "pps_paths.h"
#include "pps_io.h"
#include "pps_jni.h"
#include "pps_platform.h"
#include "pps_purchases.h"
#include "pps_music.h"
#include "pps_assets.h"
#include "pps_input.h"
#include "nx_pointer.h"
#include "compat_stubs.h"
#include "pps_shim.h"
#include "imports.h"
#include "opensles.h"

extern DynLibFunction dynlib_functions[];
extern const size_t dynlib_numfunctions;

/* ------------------------------------------------------------------ */
/* The module                                                          */
/* ------------------------------------------------------------------ */

so_module pps_mod;

/* Heap reserved for the loaded module's LOAD segments.
 *
 * Sized from the real library rather than picked: the two LOAD segments are
 * 0x1a7a390 (26.5 MB, R+X) and 0x1b33b8 memsz (1.8 MB, RW), so about 28.5 MB
 * mapped. 48 MB leaves room for a different build of the game without another
 * round of tuning, and this console has the memory to spare in game-override
 * mode. It does NOT in applet mode, which is one of the reasons applet mode
 * cannot work here. */
#define SO_LOAD_SIZE (48 * 1024 * 1024)
static uint8_t *so_load_area;

/* Every thread that runs module code needs its own bionic TLS block: the
 * engine's stack-protector prologues read the canary from TPIDR_EL0+0x28.
 *
 * There are 15,198 tpidr_el0 read sites in this library's .text -- thirteen
 * times as many as in libosmos.so -- so this is emphatically not optional, and
 * it is not only the main thread. libc_shim's pthread_create_fake installs one
 * for every thread the engine spawns; this is the main thread's. */
static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ */
/* The game's JNI entry points                                         */
/* ------------------------------------------------------------------ */

/* These are INSTANCE natives, not static ones, so the ABI is
 *     (JNIEnv *env, jobject thiz, <declared args...>)
 * and the second argument is the NativeApplication object rather than a
 * jclass. The engine does not inspect it, but it must be a valid pooled
 * object -- see jni_this().
 *
 * The declared argument lists are the Java signatures straight out of the dex,
 * reproduced in the comments so the two can be diffed by eye. */

typedef long (*fn_create_inst)(void *env, void *cls, void *activity);
typedef void (*fn_create)(void *env, void *thiz, long native, void *act, void *ctx, void *assets);
typedef void (*fn_v)(void *env, void *thiz);
typedef void (*fn_i)(void *env, void *thiz, int a);
typedef void (*fn_ii)(void *env, void *thiz, int a, int b);
typedef void (*fn_iii)(void *env, void *thiz, int a, int b, int c);
typedef void (*fn_iiii)(void *env, void *thiz, int a, int b, int c, int d);
/* Returns jboolean, which is uint8_t -- NOT int.
 *
 * AAPCS64 does not require a callee to clear the upper bits of w0 when it
 * returns a type narrower than 32 bits; those bits are unspecified. Declaring
 * this as `int` therefore reads up to 24 bits of whatever the engine happened
 * to leave in the register. In practice compilers usually emit a clean 0 or 1,
 * which is exactly why this would survive testing and then fail on a different
 * build of the game -- and the failure mode is the bad one: a false (quit)
 * with dirty upper bits reads as true, so the engine's request to exit is
 * silently ignored and it keeps being stepped after it has torn itself down. */
typedef uint8_t (*fn_f_ret_z)(void *env, void *thiz, float dt);
typedef void *(*fn_ret_s)(void *env, void *thiz);
/* jint JNI_OnLoad(JavaVM *vm, void *reserved) -- NOT a Java_* native. */
typedef int32_t (*fn_onload)(void *vm, void *reserved);

static struct {
  fn_create_inst CreateNativeInstance;  /* PlatformProxy: (Activity)J -- STATIC */
  fn_create      Create;      /* (JLandroid/app/Activity;Landroid/content/Context;Landroid/content/res/AssetManager;)V */
  fn_iii         Init;        /* (III)V  -- width, height, rotation */
  fn_ii          UpdateScreenSize; /* (II)V */
  fn_i           UpdateOrientation; /* (I)V */
  fn_f_ret_z     Step;        /* (F)Z    -- seconds; false means quit */
  fn_v           Destroy;     /* ()V */
  fn_v           ShuttingDown;/* ()V */
  fn_iiii        TouchEvent;  /* (IIII)V -- id, type, x, y */
  fn_i           SystemEvent; /* (I)V    -- ESystemEvent ordinal */
  fn_v           BackDown;    /* ()V */
  fn_v           BackUp;      /* ()V */
  fn_ret_s       CrashReport; /* ()Ljava/lang/String; */
  fn_v           RemoveCrash; /* ()V */
  fn_onload      OnLoad;      /* JNI_OnLoad(JavaVM*, void*) */
} g;

#define NA "Java_com_king_core_NativeApplication_"

/* so_find_addr_rx aborts on a missing symbol, which is what we want for the
 * entry points the loop cannot run without: a clear name beats a fault. */
#define BIND(field, name) \
  g.field = (void *)so_find_addr_rx(&pps_mod, name)
#define BIND_OPT(field, name) \
  g.field = (void *)so_try_find_addr_rx(&pps_mod, name)

static void bind_entrypoints(void) {
  /* JNI_OnLoad is NOT optional, and it is the one entry point that has no
   * Java_ prefix because Java never called it -- the dynamic linker did.
   * See the call site in load_module for what happens without it. */
  BIND(OnLoad, "JNI_OnLoad");

  BIND(CreateNativeInstance,
       "Java_com_midasplayer_apps_papapearsaga_PlatformProxy_createNativeInstance");
  BIND(Create,            NA "create");
  BIND(Init,              NA "init");
  BIND(UpdateScreenSize,  NA "updateScreenSize");
  BIND(Step,              NA "step");
  BIND(TouchEvent,        NA "onTouchEvent");

  /* Present in this build, but the loop copes without them, so a different
   * build of the game does not have to abort at load. */
  BIND_OPT(UpdateOrientation, NA "updateOrientation");
  BIND_OPT(Destroy,           NA "destroy");
  BIND_OPT(ShuttingDown,      NA "setAppShuttingDownFlag");
  BIND_OPT(SystemEvent,       NA "onSystemEvent");
  BIND_OPT(BackDown,          NA "onBackKeyDown");
  BIND_OPT(BackUp,            NA "onBackKeyUp");
  BIND_OPT(CrashReport,       NA "getCrashReportIfAvailable");
  BIND_OPT(RemoveCrash,       NA "removeCrashReport");
}

static void send_system_event(int ordinal) {
  if (!g.SystemEvent) return;
  LOGB("system event %d", ordinal);
  g.SystemEvent(jni_env(), jni_this(), ordinal);
}

/* ------------------------------------------------------------------ */
/* EGL                                                                 */
/* ------------------------------------------------------------------ */

static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLSurface egl_surf = EGL_NO_SURFACE;
static EGLContext egl_ctx = EGL_NO_CONTEXT;

static int egl_start(void) {
  EGLConfig cfg; EGLint n = 0;
  EGLint r = 0, gg = 0, b = 0, a = 0, d = 0, s = 0;

  /* The engine renders its scene into an offscreen FBO and composites it, and
   * its shaders ask for depth and stencil -- the FF renderer uses stencil for
   * the UI's clip rectangles, which is what scrollable panels are built from.
   * Omitting stencil does not fail to create a surface; it makes every clipped
   * panel draw over its own borders. */
  static const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      24,
    EGL_STENCIL_SIZE,    8,
    EGL_NONE
  };
  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

  egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl_dpy == EGL_NO_DISPLAY) { LOGE("eglGetDisplay failed"); return 0; }
  if (!eglInitialize(egl_dpy, NULL, NULL)) { LOGE("eglInitialize failed"); return 0; }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { LOGE("eglBindAPI failed"); return 0; }

  if (!eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
    LOGE("eglChooseConfig found no config with depth+stencil"); return 0;
  }

  /* Ask for the render size explicitly. The default window is the panel's
   * 1280x720 in handheld; without this, requesting 1080p would silently get
   * 720p and every derived value -- the UI scale the engine computes from the
   * size we hand init(), the offscreen FBO, the touch mapping -- would be
   * built for a surface that does not exist. */
  if (R_FAILED(nwindowSetDimensions(nwindowGetDefault(),
                                    PPS_RENDER_W, PPS_RENDER_H)))
    LOGW("nwindowSetDimensions(%d, %d) failed; the surface may be 720p",
         PPS_RENDER_W, PPS_RENDER_H);

  egl_surf = eglCreateWindowSurface(egl_dpy, cfg, nwindowGetDefault(), NULL);
  if (egl_surf == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return 0; }

  egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (egl_ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return 0; }

  if (!eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) {
    LOGE("eglMakeCurrent failed"); return 0;
  }
  eglSwapInterval(egl_dpy, cfg_vsync() ? 1 : 0);

  /* Log what we actually got. If the context silently came up on a software
   * path or without stencil, every later symptom is downstream of this line
   * and unexplainable without it. */
  eglGetConfigAttrib(egl_dpy, cfg, EGL_RED_SIZE,     &r);
  eglGetConfigAttrib(egl_dpy, cfg, EGL_GREEN_SIZE,   &gg);
  eglGetConfigAttrib(egl_dpy, cfg, EGL_BLUE_SIZE,    &b);
  eglGetConfigAttrib(egl_dpy, cfg, EGL_ALPHA_SIZE,   &a);
  eglGetConfigAttrib(egl_dpy, cfg, EGL_DEPTH_SIZE,   &d);
  eglGetConfigAttrib(egl_dpy, cfg, EGL_STENCIL_SIZE, &s);
  LOGB("EGL config  R%d G%d B%d A%d  depth %d  stencil %d", r, gg, b, a, d, s);
  LOGB("GL_VENDOR   %s", (const char *)glGetString(GL_VENDOR));
  LOGB("GL_RENDERER %s", (const char *)glGetString(GL_RENDERER));
  LOGB("GL_VERSION  %s", (const char *)glGetString(GL_VERSION));
  return 1;
}

static void egl_stop(void) {
  if (egl_dpy == EGL_NO_DISPLAY) return;
  eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (egl_ctx)  eglDestroyContext(egl_dpy, egl_ctx);
  if (egl_surf) eglDestroySurface(egl_dpy, egl_surf);
  eglTerminate(egl_dpy);
  egl_dpy = EGL_NO_DISPLAY; egl_surf = EGL_NO_SURFACE; egl_ctx = EGL_NO_CONTEXT;
}

/* ------------------------------------------------------------------ */
/* Loading the module                                                  */
/* ------------------------------------------------------------------ */

static int load_module(void) {
  startup_status_update("Reserving memory");
  so_load_area = memalign(0x1000, SO_LOAD_SIZE);
  if (!so_load_area)
    fatal_error("Could not reserve %d MB for the game library.\n\n"
                "This needs to run as a title override (hold R while starting\n"
                "an installed game), not in applet mode -- applet mode gets a\n"
                "small fraction of the console's memory.",
                SO_LOAD_SIZE / (1024 * 1024));

  startup_status_update("Loading libpapapearsaga.so");
  if (so_load(&pps_mod, pps_so_path(), so_load_area, SO_LOAD_SIZE) < 0)
    fatal_error("Could not load %s.\n\nIt may be truncated, or it may be the\n"
                "32-bit build -- this port needs the arm64-v8a library.",
                pps_so_path());

  startup_status_update("Relocating");
  if (so_relocate(&pps_mod) < 0)
    fatal_error("so_relocate failed. The library is not the expected format.");

  startup_status_update("Resolving imports");
  /* The library is linked BIND_NOW, so every import must resolve before the
   * first call into it. taint_missing_imports = 1 makes an unresolved symbol
   * abort with its name at the moment it is called, rather than jumping to
   * address 0 and faulting somewhere unrelated. */
  if (so_resolve(&pps_mod, dynlib_functions, (int)dynlib_numfunctions, 1) < 0)
    fatal_error("so_resolve failed: the library imports a symbol this port\n"
                "does not provide. Run:\n\n"
                "  make check SO=/path/to/libpapapearsaga.so\n\n"
                "to see which one.");

  /* ORDER MATTERS HERE, and getting it wrong is a Data Abort at boot.
   *
   * so_load only RESERVES the virtual range (virtmemFindCodeMemory +
   * virtmemAddReservation). A reservation is bookkeeping; nothing is mapped at
   * load_virtbase yet. so_finalize is what performs the
   * svcMapProcessCodeMemory that aliases load_base to load_virtbase and sets
   * the page permissions.
   *
   * So so_flush_caches, which touches load_virtbase directly, MUST come after
   * so_finalize. This file had them the other way round: so_util.c catches it
   * with an explicit message rather than faulting inside armDCacheFlush with
   * nothing pointing back at the loader, which is the only reason it was a
   * legible error rather than a mystery crash.
   *
   * Anything that WRITES to the module must go the other side of that line --
   * before so_finalize, while load_base is still ordinary writable memory. The
   * kernel never permits a W->X transition on code memory once mapped, so a
   * patch applied afterwards either faults or silently fails. That is why
   * so_patch_stack_canaries is not called here; see below. */

  /* so_patch_stack_canaries is deliberately NOT called.
   *
   * It would have to run before so_finalize, and this file had it after --
   * writing to .text that had just been mapped RX. But it should not run at
   * all. The engine's prologues read the guard from TPIDR_EL0+0x28 and its
   * epilogues compare against the same slot, and install_bionic_tls gives
   * every thread its own zeroed block, so the value is consistent for the
   * life of the thread and the checks pass on their own. Neither reference
   * port patches them either; one documents that NOPing thousands of b.ne
   * sites risks matching branches that are not canary checks at all. With
   * 15,198 tpidr_el0 sites in this library that risk is larger here, not
   * smaller. */

  startup_status_update("Finalising");
  so_finalize(&pps_mod);
  so_flush_caches(&pps_mod);

  startup_status_update("Running static initialisers");
  /* 240 of them in this library, and they run real engine code -- allocators,
   * the logging system, the FF resource registry. Everything the shims provide
   * has to be up before this line, which is why jni_init and the music mixer
   * are initialised first in main().
   *
   * They do NOT need a GL context. On Android this was System.loadLibrary from
   * Activity.onCreate, which runs before the GLSurfaceView has one -- so
   * nothing here can have depended on GL. That is what makes it safe to load
   * the module before egl_start(). */
  so_execute_init_array(&pps_mod);

  bind_entrypoints();

  /* JNI_OnLoad, and this is what the first hardware run died on.
   *
   * On Android, System.loadLibrary does two things: it runs the library's
   * initialisers, and then it calls JNI_OnLoad(vm, NULL). The second half is
   * where the engine captures the JavaVM* into a file-static and caches it for
   * the life of the process -- every later "get me the JNIEnv for this thread"
   * goes through that pointer.
   *
   * This port did the first half and not the second, so the pointer stayed
   * NULL. createNativeInstance does not need it and returned a healthy object;
   * create() does, and its first act is a helper that does
   *
   *     (*g_vm)->GetEnv(g_vm, &env, JNI_VERSION_1_6)
   *
   * which loads the vtable from address 0. The crash report is exactly that:
   * Data Abort at 0, X[00] = 0, X[02] = 0x10006 (JNI_VERSION_1_6 already in
   * place as the third argument), and the faulting PC sitting in an unexported
   * static immediately after JNI_OnLoad in .text -- the same translation unit,
   * sharing the global JNI_OnLoad was supposed to have written.
   *
   * It must come AFTER the init array (the C++ globals it touches have to be
   * constructed) and BEFORE any entry point. */
  startup_status_update("JNI_OnLoad");
  {
    const int32_t v = g.OnLoad(jni_vm(), NULL);
    LOGB("boot: JNI_OnLoad returned 0x%08x", (unsigned)v);
    /* It should hand back a JNI version. A negative value is the documented
     * way to say "I refuse to load", and continuing past that would fault
     * somewhere less obvious. */
    if (v < 0)
      fatal_error("JNI_OnLoad rejected the JNI environment (returned 0x%08x).\n\n"
                  "The engine inspected the JavaVM it was handed and would not\n"
                  "accept it.", (unsigned)v);
    if (v != 0x00010006 && v != 0x00010004 && v != 0x00010002)
      LOGW("JNI_OnLoad returned 0x%08x, which is not a JNI version; "
           "continuing anyway", (unsigned)v);
  }

  /* Only now release the raw ELF image.
   *
   * mod->syms and mod->dynstrtab are set from load_base (so_util.c:221,224),
   * so symbol lookup would still work after this -- but elf_hdr, sec_hdr and
   * shstrtab all point into the buffer being freed. Binding first means this
   * file does not depend on that distinction being right. Both reference ports
   * hold the image until exactly this point. */
  so_free_temp(&pps_mod);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static long g_native_instance;


/* A one-line marker for where boot got to.
 *
 * This is the cheapest possible version of the Osmos port's osmos_diag.c: no
 * GL wrapping, no thread watchdog, just a named phase in debug.log. It earns
 * its place because the failures this port is most likely to hit -- a static
 * initialiser faulting, a shader that will not compile, an asset root that is
 * one directory off -- all present identically as "it stopped", and the last
 * phase written is the difference between knowing and guessing. */
static void diag_phase(const char *what) {
  LOGB("--- %s ---", what);
}

static int boot_game(void) {
  void *env = jni_env();
  void *thiz = jni_this();
  void *activity = jni_make_object("com/midasplayer/apps/papapearsaga/PapaPearSagaActivity");
  void *assets = jni_make_object("android/content/res/AssetManager");

  /* 1. PlatformProxy.createNativeInstance(activity) -> jlong.
   *
   * A STATIC native, so the second argument is a jclass rather than the
   * instance -- but our objects are opaque handles and the engine does not
   * inspect either, so the same pooled object serves for both.
   *
   * The jlong it returns is the native application object, and it is passed
   * straight back into create() below. It is not a handle we can invent: the
   * engine allocates the object here and dereferences the same pointer there. */
  LOGB("boot: createNativeInstance");
  g_native_instance = g.CreateNativeInstance(env, thiz, activity);
  LOGB("boot: native instance = %p", (void *)g_native_instance);
  if (!g_native_instance)
    LOGW("createNativeInstance returned 0; create() will likely fault");

  /* 2. create(nativeInstance, activity, context, assetManager).
   *
   * This is where the engine takes the asset manager, so pps_assets must
   * already be initialised -- it is, from main(), before the init_array even
   * ran. The activity and context are the same pooled object; nothing in the
   * engine distinguishes them beyond passing them to platform calls that we
   * answer by class name anyway. */
  LOGB("boot: create()");
  diag_phase("create");
  g.Create(env, thiz, g_native_instance, activity, activity, assets);
  LOGB("boot: create() returned");

  /* 3. init(width, height, rotation), then updateScreenSize(width, height).
   *
   * Both, and in this order. onSurfaceChanged did exactly this pair, and they
   * are not redundant: init() constructs the renderer and the application from
   * the size, updateScreenSize() publishes the size to the layout system that
   * the UI reads back. Skipping the second leaves the interface laid out at
   * whatever default the engine started with, which is the classic "it renders
   * but the menu is in the wrong place" symptom. */
  LOGB("boot: init(%d, %d, %d)", PPS_RENDER_W, PPS_RENDER_H, PPS_ROTATION);
  diag_phase("init");
  g.Init(env, thiz, PPS_RENDER_W, PPS_RENDER_H, PPS_ROTATION);

  LOGB("boot: updateScreenSize(%d, %d)", PPS_RENDER_W, PPS_RENDER_H);
  g.UpdateScreenSize(env, thiz, PPS_RENDER_W, PPS_RENDER_H);

  /* 4. The lifecycle events the Java activity would have queued on resume.
   *
   * GameActivity.onResume sent WILL_ENTER_FOREGROUND then DID_BECOME_ACTIVE,
   * in that order, around GameView.onResume. The engine gates its audio and
   * its update loop on having seen DID_BECOME_ACTIVE, so without these it
   * boots into a paused state and renders a static first frame forever. */
  send_system_event(PPS_EV_WILL_ENTER_FOREGROUND);
  send_system_event(PPS_EV_DID_BECOME_ACTIVE);

  /* 5. Restore what the player owns. After create(), because the store lives
   * on the native application object, and after the lifecycle events, because
   * the engine processes store callbacks on its own queue which only drains
   * once it is active. */
  if (cfg_purchases_enabled() && pps_purchases_count() > 0) {
    LOGB("boot: restoring %d purchase(s)", pps_purchases_count());
    pps_purchases_deliver(&pps_mod, env, thiz);
  }

  /* The single most useful line in the log, and it belongs here rather than at
   * the first miss: if this reads in the thousands the asset root is wrong and
   * nothing downstream will work, whereas a handful is the engine probing for
   * optional per-language and A/B assets, which is normal. */
  {
    const int misses = pps_assets_miss_count();
    if (misses > 500)
      LOGW("boot: %d asset opens found nothing (first: %s).\n"
           "      That is far too many -- check that assets/res_output sits\n"
           "      inside %s and came out of the APK intact.",
           misses, pps_assets_first_miss(), pps_assets_root());
    else
      LOGB("boot: %d asset opens found nothing (normal: optional assets)", misses);
  }

  /* What the engine asked for through GameLib.setupPlatform, against what it
   * was actually given. It has no way to refuse our size, so a mismatch is not
   * an error -- but it is the engine telling us what it expected, and if the
   * UI turns out to be laid out for the wrong dimensions this line is the
   * evidence. Previously pps_platform_setup() had no callers at all while its
   * header claimed main.c read it. */
  {
    const PpsPlatformSetup *ps = pps_platform_setup();
    if (ps->width || ps->height) {
      LOGB("boot: engine asked for %dx%d rot %d %dfps msaa %d",
           ps->width, ps->height, ps->rotation, ps->target_fps, ps->msaa);
      if (ps->width != PPS_RENDER_W || ps->height != PPS_RENDER_H)
        LOGW("engine asked for %dx%d but the surface is %dx%d",
             ps->width, ps->height, PPS_RENDER_W, PPS_RENDER_H);
    } else {
      LOGB("boot: the engine never called setupPlatform");
    }
  }

  LOGB("boot: entering frame loop");
  diag_phase("frame loop");
  return 1;
}

/* ------------------------------------------------------------------ */
/* Frame loop                                                          */
/* ------------------------------------------------------------------ */

static void frame_loop(void) {
  /* The first onDrawFrame returned without stepping -- GameRenderer guarded it
   * with mFirstDrawFrame. Reproduced: the engine completes GL setup on the
   * render thread during that frame. */
  int first_frame = 1;
  int last_rotation = PPS_ROTATION;
  int focused = 1;
  u64 last_tick = armGetSystemTick();

  while (appletMainLoop()) {
    float dt;
    u64 now;
    uint8_t keep_going;

    /* Focus. appletGetFocusState tells us about the home menu and the overlay
     * applets; the engine wants the same pair of events the Java lifecycle
     * would have sent, or its audio keeps playing under the home menu. */
    {
      const int now_focused = (appletGetFocusState() == AppletFocusState_InFocus);
      if (now_focused != focused) {
        focused = now_focused;
        if (focused) {
          send_system_event(PPS_EV_WILL_ENTER_FOREGROUND);
          send_system_event(PPS_EV_DID_BECOME_ACTIVE);
          /* The clock jumped while we were away. Without this the next dt is
           * however long the console sat in the home menu, and the engine
           * integrates that in one step -- every timer fires at once. */
          last_tick = armGetSystemTick();
        } else {
          pps_input_cancel_all();
          send_system_event(PPS_EV_WILL_RESIGN_ACTIVE);
          send_system_event(PPS_EV_DID_ENTER_BACKGROUND);
        }
      }
    }

    if (!focused) {
      /* Not stepping while unfocused is deliberate: the engine has been told
       * it is backgrounded and stepping it anyway would advance a game the
       * player cannot see. */
      svcSleepThread(16000000ULL);
      continue;
    }

    pps_input_poll();

    if (pps_input_back_pressed()) {
      LOGI("back pressed");
      if (g.BackDown) g.BackDown(jni_env(), jni_this());
      if (g.BackUp)   g.BackUp(jni_env(), jni_this());
    }

    if (pps_input_quit_requested()) {
      LOGB("quit gesture held; leaving the loop");
      break;
    }

    /* The rotation never changes on this console, so updateOrientation fires
     * once at most. Kept because GameRenderer called it on change and a
     * future docked/portrait experiment would want the hook already wired. */
    if (g.UpdateOrientation && last_rotation != PPS_ROTATION) {
      g.UpdateOrientation(jni_env(), jni_this(), PPS_ROTATION);
      last_rotation = PPS_ROTATION;
    }

    now = armGetSystemTick();
    /* armTicksToNs is exact for this clock; dividing ticks by a constant is
     * not -- the tick rate is 19.2 MHz, not a power of ten. */
    dt = (float)((double)armTicksToNs(now - last_tick) / 1.0e9);
    last_tick = now;

    /* Clamp. A long asset load or a debugger pause produces a dt the engine's
     * physics integrates in one step, which fires every pin at once. Half a
     * second is well past anything a healthy frame produces. */
    if (dt > 0.5f) dt = 0.5f;
    if (dt < 0.0f) dt = 0.0f;

    if (first_frame) {
      first_frame = 0;
      eglSwapBuffers(egl_dpy, egl_surf);
      continue;
    }

    keep_going = g.Step(jni_env(), jni_this(), dt);

    /* FALSE means quit. The Java renderer minimised the app on a zero return;
     * there is nothing to minimise to here, so leave the loop and shut down
     * cleanly, which at least gives the engine its chance to save. */
    if (!keep_going) {
      LOGB("step() returned false; the engine asked to exit");
      break;
    }

    if (jni_quit_requested) {
      LOGB("the engine called finish(); leaving the loop");
      break;
    }

    nxp_draw();
    eglSwapBuffers(egl_dpy, egl_surf);
  }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  /* Order here matters and is load-bearing up to boot_game().
   *
   * io_init FIRST. It builds the recursive mutex every file call in this port
   * goes through, and cfg_load and log_init both open files -- so it used to
   * run fourth, after two functions had already taken a lock that had not been
   * initialised. That works by accident on libnx, because a static RMutex is
   * zero-initialised and rmutexInit only zeroes it, but "works by accident" is
   * not something to leave in the startup path of a port that has never run.
   *
   * Then paths, because the log and the config both live in the directory it
   * finds; then the error screen's way to reclaim the display from EGL; then
   * everything the module's 240 static initialisers might touch, because those
   * run inside load_module() and there is no opportunity between. */
  io_init();
  pps_paths_init();
  cfg_load();
  log_init();
  pps_paths_log_search();
  pps_paths_make_dirs();

  error_set_gfx_release(egl_stop);

  if (!pps_paths_check()) {
    error_screen(pps_paths_error());
    log_close();
    return 0;
  }

  startup_status_begin("Starting Papa Pear Saga");

  /* The main thread's bionic TLS. Before anything can call into the module --
   * which includes the static initialisers. */
  install_bionic_tls(main_tls);

  pps_mmap_arena_init();
  jni_init();
  pps_music_init();
  pps_purchases_ensure_file();

  {
    NxpConfig nxp = {0};
    nxp.screen_w = PPS_RENDER_W; nxp.screen_h = PPS_RENDER_H;
    nxp.panel_w  = PPS_PANEL_W;  nxp.panel_h  = PPS_PANEL_H;
    nxp.data_dir = pps_root();
    nxp.max_touch_slots = PPS_MAX_TOUCHES;
    nxp.fopen_fn = fopen_locked;
    nxp.fclose_fn = fclose_locked;
    nxp_init(&nxp);
  }

  /* LOAD THE MODULE BEFORE BRINGING UP EGL, not after.
   *
   * Two reasons, and the first is a hard conflict. consoleInit() takes the
   * default window, and eglCreateWindowSurface wants the same one -- so the
   * status screen and the GL surface cannot both be up. This used to start the
   * status console, create the EGL surface underneath it, and only call
   * startup_status_end() after load_module returned.
   *
   * The second is that this order is the faithful one. On Android the library
   * was loaded by System.loadLibrary from Activity.onCreate, which runs BEFORE
   * the GLSurfaceView has a context -- so the module's 240 static initialisers
   * cannot have depended on GL, because there was none when they ran. Loading
   * first is therefore not a compromise to resolve the window conflict; it is
   * what the engine was built against. The context is needed by create() and
   * init(), which come after.
   *
   * The visible cost is that the slowest part of startup -- 28 MB of library
   * and 60,296 relocations -- now happens with the status screen up rather
   * than a black screen, which is the right way round. */
  load_module();

  startup_status_end();

  /* Now the console has given the window back, EGL can have it. */
  if (!egl_start()) {
    error_screen("Could not create an OpenGL ES 2.0 surface.\n\n"
                 "Make sure switch-mesa is installed and that you are running\n"
                 "as a title override rather than in applet mode.");
    log_close();
    return 0;
  }

  pps_input_init((pps_touch_fn)g.TouchEvent, jni_env(), jni_this());

  /* The engine's own crash report from the previous run, if it wrote one.
   * Reading and clearing it is what the Java layer did on every start, and
   * leaving it unread makes the engine believe it is still crashing. */
  if (g.CrashReport) {
    void *s = g.CrashReport(jni_env(), jni_this());
    const char *txt = s ? jni_string_utf(s) : NULL;
    if (txt && *txt) {
      LOGW("the previous run left a crash report:\n%s", txt);
      if (g.RemoveCrash) g.RemoveCrash(jni_env(), jni_this());
    }
  }

  boot_game();
  frame_loop();

  /* Shutdown, in the order GameActivity used. setAppShuttingDownFlag first --
   * it is what tells the engine's worker threads to stop before destroy()
   * frees what they are reading. */
  LOGB("shutting down");
  if (g.ShuttingDown) g.ShuttingDown(jni_env(), jni_this());
  send_system_event(PPS_EV_WILL_TERMINATE);
  if (g.Destroy) g.Destroy(jni_env(), jni_this());

  nxp_save_settings();
  pps_music_shutdown();
  opensles_shutdown();
  egl_stop();
  log_close();
  return 0;
}
