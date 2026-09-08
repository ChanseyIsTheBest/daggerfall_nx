/* pps_jni.h -- the JNI environment libpapapearsaga.so expects.
 *
 * MIT licensed. See LICENSE.
 *
 * WHY THIS IS NEITHER osmos_jni.c NOR jni_fake.c
 * ----------------------------------------------
 * The two reference ports sit at opposite ends of a spectrum and this game
 * sits between them.
 *
 * Osmos could be enumerated exactly -- ten methods over five classes -- so its
 * port hand-wrote a small, real JNI environment with no fallbacks. Sonic Jump
 * is Unity, which reaches into Java for dozens of platform classes that cannot
 * be listed ahead of time, so its port carries a ~1700-line compatibility net
 * that answers anything.
 *
 * Papa Pear Saga has a large surface -- 95 exported Java_* entry points across
 * com.king.* and com.midasplayer.* -- but it IS enumerable, because King's
 * engine talks to its own platform layer rather than to arbitrary Android.
 * Reading the dex gives the exact class and method set.
 *
 * So this file is the net, and pps_platform.c is the routing table over it:
 *
 *   - Every FindClass and GetMethodID succeeds and interns a handle, so the
 *     engine's startup never fails on a lookup.
 *   - Calls are dispatched by (class, method) to a real implementation where
 *     one exists -- FileSystem, FileLib, Device, DeviceLocale, Time, GameLib,
 *     UuidGenerator, MusicManager.
 *   - Everything else returns a typed zero. That is the correct answer for
 *     the ads, analytics, Facebook, push-notification and web-view surfaces,
 *     which have no meaning on this console.
 *
 * The net matters more than it looks. A game this size touches its platform
 * layer from static initialisers, and a null where the engine expected an
 * object is a fault during a constructor -- long before there is a frame to
 * show or a log line to read.
 */

#ifndef PPS_JNI_H
#define PPS_JNI_H

#include <stdint.h>
#include <stdarg.h>

/* Set when the engine asks the activity to finish. main.c polls it. */
extern volatile int jni_quit_requested;

void  jni_init(void);

void *jni_env(void);   /* JNIEnv * */
void *jni_vm(void);    /* JavaVM *  -- handed to JNI_OnLoad and to the engine */

/* The jobject passed as argument 2 of every instance native call. The engine
 * does not inspect it, but it has to be a valid pooled object: anything doing
 * GetObjectClass on it must not fault. */
void *jni_this(void);

/* Build a jstring the engine can read with GetStringUTFChars. Pooled by
 * content -- the engine re-creates the same constant strings constantly. */
void *jni_make_string(const char *utf8);

/* Read a jstring back out. Returns NULL for a non-string. */
const char *jni_string_utf(void *jstr);

/* Primitive arrays, for the byte[] and float[] the platform classes return. */
void *jni_make_byte_array(const void *data, int len);
void *jni_make_int_array(const int32_t *data, int len);
void *jni_make_float_array(const float *data, int len);
void *jni_make_long_array(const int64_t *data, int len);
void *jni_bytearray_data(void *arr, int *len_out);

/* Object arrays, for the Purchase[] the store restore path delivers. */
void *jni_make_object_array(int len);
void  jni_object_array_set(void *arr, int index, void *value);

/* An opaque object tagged with a class name. Dispatch is by that name, so
 * these stand in for the Java objects the engine holds -- a Purchase, a
 * SkuDetails, an Activity. */
void *jni_make_object(const char *class_name);

/* A DISTINCT object, not the pooled one. The store restore path needs several
 * Purchase objects that differ only in their properties, so they cannot share
 * the single pooled instance that stateless handles use. */
void *jni_make_unique_object(const char *class_name);

/* Attach a (key -> string) property to an opaque object, so a handler can
 * answer getSku()/getToken() on it later without a second lookup table. */
void  jni_object_set_prop(void *obj, const char *key, const char *value);
const char *jni_object_get_prop(void *obj, const char *key);

/* The class name an object was made with, or "java/lang/Object". */
const char *jni_class_name_of(void *obj);

/* ------------------------------------------------------------------ */
/* The routing interface pps_platform.c implements                     */
/* ------------------------------------------------------------------ */

/* Arguments are marshalled once, guided by the method signature, before a
 * handler sees them. Walking a raw va_list by index is not sound -- on
 * aarch64 the integer and floating-point arguments come from two separate
 * register save areas, so reading them out of order silently returns the
 * wrong one. Everything below is already unpacked in declaration order. */
#define PPS_MAX_ARGS 12

typedef struct {
  int      count;
  char     kind[PPS_MAX_ARGS];   /* the JNI signature letter: I J F D Z L [ */
  int64_t  i[PPS_MAX_ARGS];      /* integral and boolean values             */
  double   f[PPS_MAX_ARGS];      /* float and double values                 */
  void    *o[PPS_MAX_ARGS];      /* object and array references             */
} PpsArgs;

/* Returns 1 if the call was handled, 0 to fall through to the typed zero.
 * `out` carries the result: an integral in .i[0], a float in .f[0], an object
 * in .o[0], chosen by the signature's return type. */
int pps_platform_call(const char *cls, const char *name, const char *sig,
                      void *recv, const PpsArgs *args, PpsArgs *out);

/* Does this class have a handler at all? Used to decide whether an unhandled
 * call is worth logging. */
int pps_platform_owns(const char *cls);

#endif
