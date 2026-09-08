/* pps_platform.c -- the com.king.* platform classes, implemented for real.
 *
 * MIT licensed. See LICENSE.
 *
 * The engine's outbound JNI surface is large but it is not arbitrary: King's
 * C++ talks to King's own Java, and the dex says exactly what that is. Most of
 * it is ads, analytics, Facebook, push notifications and web views, which have
 * no meaning here and correctly fall through to pps_jni.c's typed zero.
 *
 * What is left is this file, and it is short:
 *
 *   FileSystem      where saves, cache and shared storage live
 *   FileLib         a complete file API the engine uses INSTEAD of fopen for
 *                   anything it considers user data
 *   Device          hardware description -- dpi, name, tablet-ness, ids
 *   DeviceLocale    language and country
 *   Time            wall clock, monotonic clock, time zone
 *   UuidGenerator   a stable install id
 *   GameLib         the engine telling the platform what it wants
 *   MusicManager    streaming music playback
 *
 * Everything here was read out of classes.dex rather than guessed, including
 * the trailing slashes on the directory getters and the argument order of
 * setupPlatform, both of which are easy to get wrong and silent when wrong.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* rmdir, for FileLib.directoryRemove */
#include <time.h>
#include <sys/stat.h>

#include "config.h"
#include "pps_jni.h"
#include "pps_paths.h"
#include "pps_io.h"
#include "pps_platform.h"

/* ------------------------------------------------------------------ */
/* What the engine told us it wants                                    */
/* ------------------------------------------------------------------ */

static PpsPlatformSetup g_setup = { PPS_RENDER_W, PPS_RENDER_H, PPS_ROTATION,
                                    PPS_TARGET_FPS_DEFAULT, PPS_MSAA_SAMPLES };

const PpsPlatformSetup *pps_platform_setup(void) { return &g_setup; }

static int g_keyboard_showing;
static int g_cursor_position;

int pps_platform_keyboard_showing(void) { return g_keyboard_showing; }

/* ------------------------------------------------------------------ */
/* FileLib                                                             */
/* ------------------------------------------------------------------ */

/* FileLib is a full file API exposed to native as static Java methods:
 * fileOpen returns a long handle, and fileRead/fileWrite take a byte[].
 *
 * WHAT THE JAVA VERSION ACTUALLY DID
 * ----------------------------------
 * Read out of the dex rather than assumed, because getting it wrong is what
 * made the first booting run find none of its assets. FileLib.fileOpen tries
 * three things, in this order:
 *
 *     AssetManager.openFd(path)     -- an uncompressed asset
 *     AssetManager.open(path)       -- a compressed one
 *     new File(path)                -- a real file, honouring the mode
 *
 * So a RELATIVE path is an ASSET path, resolved against assets/ inside the
 * APK, and only an absolute one reaches the filesystem. This port passed every
 * path straight to fopen, so a relative one was resolved against the process
 * working directory and missed:
 *
 *     FileLib.fileOpen failed: res_output/package.xml (mode 0)
 *     FileLib.fileOpen failed: res_output/layers.xml (mode 0)
 *     FileLib.fileOpen failed: res_output/cameras.xml (mode 0)
 *
 * All three exist, at exactly that path under assets/.
 *
 * Worth knowing alongside that: native never called AAssetManager_open once
 * during the entire boot. This build reaches its assets through FileLib and
 * the Java AssetManager, not through the NDK one, so THIS function is the
 * asset path -- pps_assets.c is wired and idle.
 *
 * THE MODE ARGUMENT
 * -----------------
 * Ordinals from FileLib$EFileMode's <clinit>, in declaration order:
 *
 *     0  FILE_MODE_READ
 *     1  FILE_MODE_WRITE_APPEND   creates if absent, appends
 *     2  FILE_MODE_CREATE_WRITE   deletes an existing file, creates new
 *
 * This port had 1 and 2 the wrong way round, which is the worst kind of wrong:
 * a save written with "create" would append to the previous one rather than
 * replace it, so the first save works and every one after it corrupts. */
static const char *filelib_mode(int mode) {
  switch (mode) {
    case 1:  return "ab";   /* WRITE_APPEND */
    case 2:  return "wb";   /* CREATE_WRITE -- truncates, as delete+create did */
    default: return "rb";   /* READ */
  }
}

/* "sdmc:/..." and "/..." are filesystem paths; anything else is an asset. */
static int path_is_absolute(const char *p) {
  return p && (p[0] == '/' || strchr(p, ':') != NULL);
}

/* A path made of control characters is not a missing file, it is memory
 * corruption wearing a filename.
 *
 * This exists because a whole bring-up run was spent reading "FileLib.fileOpen
 * failed: <4 bytes of pointer>" as if the game were asking for odd assets. It
 * was not: something had written past a heap block and the string pool was
 * damaged. Checking costs one pass over a short string on a path that already
 * touches the SD card, and it turns a silent corruption into a line that says
 * what it is. */
static int path_looks_corrupt(const char *p) {
  int i;
  for (i = 0; p[i] && i < 8; i++)
    if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7e)
      return 1;
  return 0;
}

static int64_t filelib_open(const char *path, int mode) {
  char full[FS_MAX_PATH];
  FILE *f = NULL;
  int n;

  if (!path || !*path) return 0;

  if (path_looks_corrupt(path)) {
    static int reported;
    if (!reported) {
      reported = 1;
      LOGE("FileLib.fileOpen was handed a path that is not text -- the JNI "
           "string pool is corrupt.\n"
           "      This is a memory bug in the port, not a missing asset. "
           "Everything after this point is unreliable.");
    }
    return 0;
  }

  if (!path_is_absolute(path)) {
    if (mode == 0) {
      /* An asset, exactly as AssetManager.open() treated it. */
      n = snprintf(full, sizeof(full), "%s/%s", pps_assets_root(), path);
      if (n > 0 && (size_t)n < sizeof(full))
        f = fopen_locked(full, "rb");
      if (f) return (int64_t)(uintptr_t)f;
    } else {
      /* A relative write. Java would have reached `new File(path)` against the
       * process working directory, which means nothing here -- and letting it
       * land in the asset tree would write into the game's own data. The
       * writable home directory is what the engine means by it. */
      n = snprintf(full, sizeof(full), "%s%s", pps_home_dir(), path);
      if (n > 0 && (size_t)n < sizeof(full))
        f = fopen_locked(full, filelib_mode(mode));
      if (f) return (int64_t)(uintptr_t)f;
    }
  }

  /* Absolute, or the asset lookup missed: the plain-file fallback, which is
   * the third thing the Java version tried. */
  f = fopen_locked(path, filelib_mode(mode));

  if (!f) LOGB("FileLib.fileOpen failed: %s (mode %d)", path, mode);
  return (int64_t)(uintptr_t)f;
}

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

/* getDpi() returns float[]. On Android this was {xdpi, ydpi, densityDpi} from
 * DisplayMetrics, and the engine divides the third by 160 to pick its asset
 * tier. Reporting 320 puts it in the xhdpi bucket, which is the tier this APK
 * actually ships art for -- claiming a higher one makes every sprite miss. */
static void *device_dpi_array(void) {
  const float dpi[3] = { PPS_DPI, PPS_DPI, PPS_DPI };
  return jni_make_float_array(dpi, 3);
}

/* getCpuStat() returns long[]. Used only for a performance telemetry event
 * that goes nowhere here. Zeros are a valid reading. */
static void *device_cpu_stat(void) {
  const int64_t z[4] = { 0, 0, 0, 0 };
  return jni_make_long_array(z, 4);
}

/* getMacAddress() returns int[]. It is a device-identity fallback; feeding it
 * the install uuid keeps the identity stable without inventing a MAC that
 * could collide with a real one. */
static void *device_mac(void) {
  const char *u = pps_device_uuid();
  int32_t m[6];
  int i;
  for (i = 0; i < 6; i++)
    m[i] = (int32_t)((unsigned char)u[i % 32] & 0xff);
  return jni_make_int_array(m, 6);
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

/* getElapsedRealTime() is Android's SystemClock.elapsedRealtime: milliseconds
 * since boot, monotonic, and NOT affected by the wall clock. The engine uses
 * it for animation and for its own frame accounting, so it must not be wall
 * time -- a clock adjustment mid-session would otherwise make the game jump. */
static int64_t elapsed_real_ms(void) {
  return (int64_t)(armTicksToNs(armGetSystemTick()) / 1000000ULL);
}

/* getTimeZoneOffset() is milliseconds east of UTC. Read from the console's own
 * setting rather than assumed: the game gates its daily-life refill on local
 * midnight, so an hour of error is an hour of wrong lives. */
static int64_t timezone_offset_ms(void) {
  time_t now = time(NULL);
  struct tm local, utc;
  double diff;
  if (!localtime_r(&now, &local) || !gmtime_r(&now, &utc)) return 0;
  diff = difftime(mktime(&local), mktime(&utc));
  return (int64_t)(diff * 1000.0);
}

/* ------------------------------------------------------------------ */
/* Class ownership                                                     */
/* ------------------------------------------------------------------ */

static const char *const OWNED[] = {
  "com/king/core/FileSystem",
  "com/king/core/FileLib",
  "com/king/core/Device",
  "com/king/core/DeviceLocale",
  /* Not looked up by this build -- "com/king/core/Time" appears nowhere in
   * libpapapearsaga.so, so the engine gets its clock natively. Kept because
   * answering it costs nothing and another build may ask. */
  "com/king/core/Time",
  "com/king/core/UuidGenerator",
  "com/king/core/GameLib",
  "com/king/core/MusicManager",
  "com/king/core/BatteryStatus",
  "com/king/core/HapticManager",
  "com/king/core/Dialog",
  "com/king/store/GooglePlayIABv3Lib",
  "com/king/store/billingutil/Purchase",
  "com/king/store/billingutil/SkuDetails",
  "com/midasplayer/apps/papapearsaga/PlatformProxy",
  NULL
};

int pps_platform_owns(const char *cls) {
  int i;
  if (!cls) return 0;
  for (i = 0; OWNED[i]; i++)
    if (!strcmp(cls, OWNED[i])) return 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

#define IS(c)  (!strcmp(cls, (c)))
#define M(n)   (!strcmp(name, (n)))
#define RET_I(v) do { out->i[0] = (int64_t)(v); return 1; } while (0)
#define RET_F(v) do { out->f[0] = (double)(v);  return 1; } while (0)
#define RET_O(v) do { out->o[0] = (v);          return 1; } while (0)
#define RET_S(v) do { out->o[0] = jni_make_string(v); return 1; } while (0)
#define RET_V()  do { return 1; } while (0)

int pps_platform_call(const char *cls, const char *name, const char *sig,
                      void *recv, const PpsArgs *a, PpsArgs *out) {
  if (!cls || !name) return 0;
  (void)sig; (void)recv;

  /* ---------------- FileSystem ---------------- */
  if (IS("com/king/core/FileSystem")) {
    /* All three end with '/'. The Java versions appended one explicitly and
     * the engine concatenates a bare filename onto the result, so dropping it
     * silently writes the save one directory up. */
    if (M("getHomeDirectory"))   RET_S(pps_home_dir());
    if (M("getCacheDirectory"))  RET_S(pps_cache_dir());
    if (M("getSharedDirectory")) RET_S(pps_shared_dir());
  }

  /* ---------------- FileLib ---------------- */
  if (IS("com/king/core/FileLib")) {
    if (M("fileOpen"))
      RET_I(filelib_open(jni_string_utf(a->o[0]), (int)a->i[1]));

    if (M("fileClose")) {
      FILE *f = (FILE *)(uintptr_t)a->i[0];
      if (f) fclose_locked(f);
      RET_V();
    }
    if (M("fileRead")) {
      FILE *f = (FILE *)(uintptr_t)a->i[0];
      int len = 0;
      void *buf = jni_bytearray_data(a->o[1], &len);
      if (!f || !buf) RET_I(-1);
      RET_I((int)fread_locked(buf, 1, (size_t)len, f));
    }
    if (M("fileWrite")) {
      FILE *f = (FILE *)(uintptr_t)a->i[0];
      int len = 0;
      void *buf = jni_bytearray_data(a->o[1], &len);
      if (!f || !buf) RET_I(-1);
      RET_I((int)fwrite_locked(buf, 1, (size_t)len, f));
    }
    if (M("fileSeek")) {
      FILE *f = (FILE *)(uintptr_t)a->i[0];
      if (!f) RET_I(0);
      RET_I(fseek_locked(f, (long)a->i[1], SEEK_SET) == 0);
    }
    if (M("fileGetSize")) {
      FILE *f = (FILE *)(uintptr_t)a->i[0];
      long here, end;
      if (!f) RET_I(0);
      here = ftell_locked(f);
      fseek_locked(f, 0, SEEK_END);
      end = ftell_locked(f);
      fseek_locked(f, here, SEEK_SET);
      RET_I((int)end);
    }
    if (M("available")) {
      FILE *f = (FILE *)(uintptr_t)a->i[0];
      long here, end;
      if (!f) RET_I(0);
      here = ftell_locked(f);
      fseek_locked(f, 0, SEEK_END);
      end = ftell_locked(f);
      fseek_locked(f, here, SEEK_SET);
      RET_I(end - here);
    }
    if (M("flush") || M("flushAll")) RET_V();
    if (M("fileRemove"))     RET_I(remove(jni_string_utf(a->o[0])) == 0);
    if (M("directoryCreate")) {
      const char *p = jni_string_utf(a->o[0]);
      struct stat st;
      if (!p) RET_I(0);
      if (mkdir(p, 0777) == 0) RET_I(1);
      RET_I(stat(p, &st) == 0);      /* already there counts as success */
    }
    if (M("directoryRemove")) RET_I(rmdir(jni_string_utf(a->o[0])) == 0);
    /* getAppAPKPath: the engine uses this to open the APK as a zip when it
     * wants an asset the manager did not have. There is no APK here and the
     * assets are loose, so an empty string sends it down the "no APK" path
     * rather than making it open a file that is not a zip. */
    if (M("getAppAPKPath")) RET_S("");
  }

  /* ---------------- Device ---------------- */
  if (IS("com/king/core/Device")) {
    if (M("getDpi"))          RET_O(device_dpi_array());
    if (M("isTablet"))        RET_I(PPS_IS_TABLET);
    if (M("getDeviceName"))   RET_S("Nintendo Switch");
    if (M("getAppName"))      RET_S("Papa Pear Saga");
    if (M("getDeviceId"))     RET_S(pps_device_uuid());
    if (M("getCpuStat"))      RET_O(device_cpu_stat());
    if (M("getMacAddress"))   RET_O(device_mac());
    if (M("getCpuInfo"))      RET_S("aarch64 Cortex-A57");
    /* Deliberately empty rather than invented. The engine branches on
     * "installed from Google Play" for its rating prompt and its store
     * availability check; an empty installer means neither fires, which is
     * the truthful answer and also the one that keeps the store local. */
    if (M("getInstallerPackageName")) RET_S("");
    if (M("getNetworkCountryIso") || M("getSimCountryIso")) RET_S(cfg_country());
    if (M("getNetworkOperator")) RET_S("");
    if (M("initContext"))     RET_V();
  }

  /* ---------------- DeviceLocale ---------------- */
  if (IS("com/king/core/DeviceLocale")) {
    if (M("getLanguageCode")) RET_S(cfg_language());
    if (M("getCountryCode"))  RET_S(cfg_country());
  }

  /* ---------------- Time ---------------- */
  if (IS("com/king/core/Time")) {
    if (M("getElapsedRealTime"))  RET_I(elapsed_real_ms());
    if (M("getTimeZoneOffset"))   RET_I(timezone_offset_ms());
    if (M("getTimeZone"))         RET_S("UTC");
  }

  /* ---------------- UuidGenerator ---------------- */
  if (IS("com/king/core/UuidGenerator")) {
    if (M("getUuid")) RET_S(pps_device_uuid());
  }

  /* ---------------- GameLib ---------------- */
  if (IS("com/king/core/GameLib")) {
    /* setupPlatform(width, height, rotation, targetFps, msaaSamples).
     * Argument order confirmed from PlatformSetup.<init>'s field stores, not
     * from the getter names -- getMSAASamples sorts before getRotation
     * alphabetically and would have suggested the wrong order. */
    if (M("setupPlatform")) {
      g_setup.width      = (int)a->i[0];
      g_setup.height     = (int)a->i[1];
      g_setup.rotation   = (int)a->i[2];
      g_setup.target_fps = (int)a->i[3];
      g_setup.msaa       = (int)a->i[4];
      LOGB("GameLib.setupPlatform(%d, %d, rot %d, %d fps, msaa %d)",
           g_setup.width, g_setup.height, g_setup.rotation,
           g_setup.target_fps, g_setup.msaa);
      RET_V();
    }
    if (M("hideApplication")) { jni_quit_requested = 1; RET_V(); }

    /* The soft keyboard. Papa Pear Saga only asks for it on the King account
     * screens, which cannot be reached with networking off, so this reports
     * "not showing" and does nothing. Wiring it to libnx's swkbd would mean
     * blocking the render thread inside a JNI call, which is a deadlock
     * waiting to happen -- swkbd wants to own the display. */
    if (M("showKeyboard") || M("justShowKeyboard")) {
      LOGB("GameLib: soft keyboard requested; not implemented");
      RET_V();
    }
    if (M("hideKeyboard"))    { g_keyboard_showing = 0; RET_V(); }
    if (M("isKeyboardShowing")) RET_I(g_keyboard_showing);
    if (M("shouldAutoHideKeyboardOnSubmit")) RET_I(1);

    if (M("getCursorPosition")) RET_I(g_cursor_position);
    if (M("setCursorPosition")) { g_cursor_position = (int)a->i[0]; RET_V(); }

    /* Orientation is fixed. Accepting the call and ignoring it is right: the
     * engine sets this once at startup and never reads it back. */
    if (M("setAllowedOrientations")) RET_V();

    /* The accelerometer. There is no useful mapping from a console to a phone
     * being tilted, and this game only uses it for an idle-state easter egg. */
    if (M("initAccelerometer") || M("releaseAccelerometer")) RET_V();

    /* runOnGameThread: on Android this posted a Runnable to the GL thread.
     * We ARE the GL thread by the time the engine calls this, and we have no
     * way to invoke a Java Runnable anyway, so it is dropped. The engine uses
     * it only to marshal UI-thread callbacks that no longer exist. */
    if (M("runOnGameThread")) RET_V();

    if (M("showToast")) {
      overlay_note(jni_string_utf(a->o[1]));
      RET_V();
    }
    if (M("logInfo") || M("logWarning")) {
      LOGB("[GameLib] %s", jni_string_utf(a->o[1]));
      RET_V();
    }
  }

  /* ---------------- BatteryStatus / HapticManager ---------------- */
  if (IS("com/king/core/BatteryStatus")) {
    /* A console on a dock is not meaningfully "charging" from the game's
     * point of view, and the only thing this drives is a low-battery warning
     * the platform already shows. Always full, never charging. */
    if (M("getBatteryLevel")) RET_F(1.0f);
    if (M("isCharging"))      RET_I(0);
    if (M("getFlagUpdated"))  RET_I(0);
    if (M("release"))         RET_V();
  }
  if (IS("com/king/core/HapticManager")) {
    /* HID rumble would be the faithful thing, but this engine calls vibrate()
     * on every pin collision -- dozens a second during a cascade -- and the
     * Android durations are tuned for a phone's linear actuator. Left off
     * rather than shipped feeling wrong; hasVibrator() false stops the engine
     * calling vibrate at all. */
    if (M("hasVibrator")) RET_I(0);
    if (M("vibrate"))     RET_V();
  }

  /* ---------------- PlatformProxy ---------------- */
  if (IS("com/midasplayer/apps/papapearsaga/PlatformProxy")) {
    if (M("setTargetFps")) { g_setup.target_fps = (int)a->i[0]; RET_V(); }
    if (M("isAppInstalled")) RET_I(0);   /* nothing else is installed here */
    if (M("removeSplashScreen") || M("showSplashScreen")) RET_V();
    if (M("send2FbMessenger") || M("shareGameRecording")) RET_V();
  }

  /* ---------------- MusicManager ---------------- */
  if (IS("com/king/core/MusicManager"))
    return pps_music_call(name, sig, a, out);

  /* ---------------- store ---------------- */
  if (IS("com/king/store/GooglePlayIABv3Lib") ||
      IS("com/king/store/billingutil/Purchase") ||
      IS("com/king/store/billingutil/SkuDetails"))
    return pps_purchases_call(cls, name, sig, recv, a, out);

  return 0;
}
