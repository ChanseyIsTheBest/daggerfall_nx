/* jni_fake.h -- fake JNI environment for the MVGL engine (libcrx.so)
 *
 * libcrx.so is a NativeActivity app: the glue hands it the JavaVM + activity
 * jobject from the ANativeActivity struct, and the engine then drives three
 * Java helpers entirely through JNI:
 *   local/mediav/Text2Bitmap   -> draw text into an android.graphics.Bitmap
 *   local/mediav/MoviePlayer   -> FMV playback
 *   local/mediav/MyNativeActivity -> asset manager, orientation, IME, store...
 * We provide a functional JNIEnv/JavaVM so those calls resolve to native code.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to finish
extern volatile int jni_quit_requested;

void jni_init(void);

// the fake MyNativeActivity jobject handed to ANativeActivity.clazz
void *jni_make_activity_object(void);

// fake Java object / string constructors
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);


/* Object arrays, for handlers in other translation units (File.listFiles). */
void *jni_make_object_array(int len);
void *jni_make_int_array(const int *src, int len);
void *jni_make_bool_array_filled(int len, int value);
int   jni_array_length(void *arr);
void *jni_classloader_obj(void);        /* the one ClassLoader object we hand out */
void  jni_object_array_set(void *arr, int i, void *v);

/* Diagnostics (jni_fake.c). Safe to call with the features compiled out --
 * they become no-ops rather than disappearing, so callers need no #ifdef. */
void     jni_approx_summary(const char *why);
void     jni_note_approx(const char *kind, const char *cls,
                         const char *name, const char *sig);
unsigned nx_jni_retired(void);   /* references passed through the quarantine */

#endif