/* jni_fake.c -- fake JNI environment for Daggerfall Unity
 *                (Unity 2022.3.62f3 / IL2CPP, arm64).
 *
 * ADOPTED WHOLESALE FROM clonehero_nx, which is the same Unity version and the
 * same code lineage, and which has BOOTED TWICE on hardware. Our previous
 * jni_fake.c was the PvZ base plus the Killer Bean hardening plus one fix
 * back-ported from here -- i.e. a strict subset of this file's history, with
 * nothing Daggerfall-specific in it to lose.
 *
 * WHAT THIS BUYS (all of it exercised by Daggerfall):
 *   - com.unity3d.player.ReflectionHelper. Every AndroidJavaObject.Call /
 *     Get / CallStatic / GetStatic in Unity resolves through it. Daggerfall's
 *     dump.cs has 112 AndroidJavaObject/AndroidJavaClass references, so this
 *     path is live here; unhandled, it approximates to opaque objects and
 *     GetStatic<string> yields null.
 *   - Real java.io.File objects. They used to be label-only, with every
 *     getAbsolutePath() returning the data root regardless of which File was
 *     asked. Daggerfall does a great deal of file I/O.
 *   - Environment.DIRECTORY_* constants.
 *   - The String methods (equals and siblings, real hashCode) whose absence
 *     hung Clone Hero's first boot on Unity's storage check.
 *
 * WHAT WAS CHANGED: the gate macros (CH_ -> DFU_) and this header. Nothing
 * else. Clone-Hero-specific paths that remain (Rewired enumeration, OBB /
 * PlayAssetDelivery, streamingAssets.yml) are unreachable for this game rather
 * than wrong for it -- they are keyed on class and method names Daggerfall
 * never asks for -- so they are left in place rather than half-removed.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   /* strcasecmp: String.equalsIgnoreCase */
#include <pthread.h>
#include <time.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "nx_data_root.h"   /* nx_path: streamingAssets.yml for Application.version */
#include "data.h"
#include "text2bitmap.h"
#include "movie_player.h"
#include "editbox.h"
#include "android_native_unity.h"
#include "jni_unimpl.h"
#include "libc_shim.h"   /* managed_path: device-less paths for managed code */
#include "opensles.h"    /* audio_fmod_open/write: FMOD native-audio output sink */

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

void fmod_audio_start(void); // defined below; launched from FMODAudioDevice.start()

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
  // text2bitmap.h BITMAP_TAG ('BMP1') is also handled by free_ref
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; uint32_t pooled; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// validated reference access
//
// Adopted from the Killer Bean port, whose comments record the hardware
// crashes that produced it. The problem it solves: every jobject the engine
// hands back came from us originally, but by the time it comes back it may be
// stale (freed under a PopLocalFrame), foreign (a pointer the engine
// manufactured), or a stale stack slot read through a varargs signature that
// promised more arguments than were passed. A raw `*(uint32_t *)ref` on any of
// those is an immediate data abort.
//
// nx_tag_of() makes a bad pointer SURVIVABLE, not CORRECT. It returns 0, which
// matches no TAG_*, so every existing comparison takes its "not mine" branch
// instead of faulting -- and says so once, in the log, with the tag spelled in
// ASCII so an unfamiliar value is identifiable at a glance.
// ---------------------------------------------------------------------------

/* EVERY tag this loader creates, across ALL files -- not just this one.
 * Getting this list wrong is silent and destructive: an unlisted tag makes
 * nx_tag_of() answer 0, so the owning subsystem quietly stops working rather
 * than crashing. Three live outside this file and are spelled by value because
 * their headers are not all in scope; if one changes, the tag-warn line names
 * it in ASCII:
 *   BITMAP_TAG 0x424d5031 ('BMP1') text2bitmap.h
 *   UJ_TAG     0x554a4831 ('UJH1') unity_jni.c:40    Unity JNI handles
 *   UI_TAG     0x55494531 ('UIE1') unity_input.c:11  input events
 */
static int nx_tag_known(uint32_t t) {
  return t == TAG_OBJECT || t == TAG_STRING || t == TAG_OBJARR ||
         t == TAG_PRIARR || t == TAG_ID     || t == TAG_CLASS  ||
         t == BITMAP_TAG ||
         t == 0x554a4831u /* UJ_TAG, unity_jni.c */ ||
         t == 0x55494531u /* UI_TAG, unity_input.c */;
}

static void nx_tag_warn(const void *p, uint32_t t, const char *why) {
  static uint32_t seen[16]; static int nseen = 0; static int nbad = 0;
  if (!t) {                       /* unusable pointer -- no tag to key on */
    if (nbad < 8) { nbad++;
      debugPrintf("[jni] tag-warn: %s ptr=%p (ref not created by us)\n", why, p); }
    return;
  }
  for (int i = 0; i < nseen; i++) if (seen[i] == t) return;   /* once per tag */
  if (nseen < 16) {
    seen[nseen++] = t;
    char a[5] = { (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0 };
    for (int i = 0; i < 4; i++) if (a[i] < 32 || a[i] > 126) a[i] = '.';
    debugPrintf("[jni] tag-warn: %s ptr=%p tag=0x%08x ('%s') -- either a tag "
                "missing from nx_tag_known(), or a stale/foreign jobject\n",
                why, p, t, a);
  }
}

/* Cheap plausibility test, for places that only need "could this be a ref?".
 *
 * 4-aligned, NOT 8. FakeID is 324 bytes and FakeClass is 100; both are pooled
 * in arrays whose stride is not a multiple of 8, so every odd-indexed entry
 * lands on a 4-aligned address. Demanding 8 would reject half of every pooled
 * class and method reference -- GetObjectArrayElement would return NULL for
 * arrays the engine depends on. 4 still rejects freed-memory poison. */
/* Switch user address space is 39 bits. Anything at or above 0x8000000000 is
 * not a pointer -- it is data (the crash that motivated this bound was
 * 0x6374696f00000000, the ASCII bytes "oitc", read out of an unsupplied
 * varargs slot and dereferenced). Same bound the allocator's free guard uses. */
#define NX_USER_AS_END 0x8000000000ull

static inline int ptr_plausible(const void *p) {
  uintptr_t v = (uintptr_t)p;
  return p && !(v & 3u) && v >= 0x1000u && v < NX_USER_AS_END;
}

/* Read a reference's tag without trusting the pointer. 0 means "not ours". */
static inline uint32_t nx_tag_of(const void *p) {
  uintptr_t v = (uintptr_t)p;
  if (!p) return 0;                              /* NULL is normal JNI -- silent */
  if ((v & 3u) || v < 0x1000u || v >= NX_USER_AS_END) { nx_tag_warn(p, 0, "unusable pointer"); return 0; }
  uint32_t t = *(volatile const uint32_t *)p;
  if (!nx_tag_known(t)) { nx_tag_warn(p, t, "unrecognised tag"); return 0; }
  return t;
}




/* Safe accessors. JNI varargs are untrusted: when a signature promises more
 * arguments than the caller passed, the extra "object" is a stale stack slot.
 * Probing its tag through a raw cast is the classic abort, so validate first
 * and return a harmless empty value rather than reading through it. */
static const char *safe_utf(void *p) {
  if (nx_tag_of(p) != TAG_STRING) return "";
  const char *u = ((FakeString *)p)->utf;
  return u ? u : "";
}

static const char *safe_class_name(void *p) {
  /* Deliberately does NOT use nx_tag_of: a non-CLASS tag here is an ordinary
   * outcome (callers ask "is this a class?"), not something to warn about. */
  uintptr_t v = (uintptr_t)p;
  if (!p || (v & 3u) || v < 0x1000u || v >= NX_USER_AS_END) return "";
  if (*(volatile uint32_t *)p != TAG_CLASS) return "";
  return ((FakeClass *)p)->name;
}

/* A char* that arrived from the engine. What this catches is not NULL -- the
 * callers handle that -- but a slot holding string DATA where an address was
 * expected, which is what a misaligned varargs read produces. */
static const char *safe_cstr(const char *p, const char *what) {
  uintptr_t v = (uintptr_t)p;
  if (!p) return "";
  if (v < 0x1000u || v > 0x0000ffffffffffffull) {
    static int nb = 0;
    if (nb < 8) { nb++;
      debugPrintf("[jni] bad %s pointer %p -- not an address\n", what, (void *)p); }
    return "";
  }
  return p;
}


// ---------------------------------------------------------------------------
// approximation ledger
//
// Most of this file answers JNI calls by GUESSING: a plausible object, a zero,
// an empty string. Usually that is fine. Sometimes the engine stores the answer
// and something breaks minutes later, far from the call.
//
// The ledger records every guessed answer, and -- the part that matters --
// marks the ones the engine came back to CHECK. Unity's AndroidJNISafe wraps
// calls in ExceptionCheck; on a real device a wrong answer would have raised
// there. So an INSPECTED entry is a call the engine cared enough about to
// verify, which makes it a far better suspect than the hundreds it ignored.
//
// For a port that has never booted this is the single most useful diagnostic
// here: it turns "something is wrong somewhere in JNI" into a short ranked list.
// Off in release (DFU_JNI_LEDGER 0) -- it costs a string compare per guess.
// ---------------------------------------------------------------------------

#if DFU_JNI_LEDGER
#define JNI_LEDGER_MAX 64
typedef struct {
  const char *kind;
  char cls[96], name[64], sig[160];
  unsigned hits;
  int inspected;
} JniApprox;

static JniApprox g_approx[JNI_LEDGER_MAX];
static int g_approx_n = 0, g_approx_drop = 0;
static Mutex g_approx_lk;
/* Armed by a guess, consumed by the next ExceptionCheck ON THE SAME THREAD --
 * without the thread key, a check following a perfectly handled call on another
 * thread would credit itself to whatever we last approximated. The key is the
 * address of a thread-local byte, so no libnx TLS symbol is needed. */
static __thread char g_approx_tls_key;
static volatile int   g_approx_last = -1;
static volatile void *g_approx_last_tls = NULL;
#endif

/* Arm/disarm per call. */
static inline void jni_approx_arm(void) {
#if DFU_JNI_LEDGER
  g_approx_last = -1;
  g_approx_last_tls = NULL;
#endif
}

/* String-keyed, so dispatchers in other translation units can record their own
 * catch-alls without needing the FakeID layout. */
void jni_note_approx(const char *kind, const char *cl, const char *nm, const char *sg) {
#if !DFU_JNI_LEDGER
  (void)kind; (void)cl; (void)nm; (void)sg;
#else
  cl = safe_cstr(cl, "cls"); nm = safe_cstr(nm, "name"); sg = safe_cstr(sg, "sig");
  mutexLock(&g_approx_lk);
  int idx = -1;
  for (int i = 0; i < g_approx_n; i++)
    if (!strcmp(g_approx[i].cls, cl) && !strcmp(g_approx[i].name, nm) &&
        !strcmp(g_approx[i].sig, sg)) { idx = i; break; }
  if (idx < 0) {
    if (g_approx_n < JNI_LEDGER_MAX) {
      idx = g_approx_n++;
      g_approx[idx].kind = kind;
      snprintf(g_approx[idx].cls,  sizeof g_approx[idx].cls,  "%s", cl);
      snprintf(g_approx[idx].name, sizeof g_approx[idx].name, "%s", nm);
      snprintf(g_approx[idx].sig,  sizeof g_approx[idx].sig,  "%s", sg);
    } else { g_approx_drop++; }
  }
  if (idx >= 0) {
    g_approx[idx].hits++;
    g_approx_last = idx;
    g_approx_last_tls = (void *)&g_approx_tls_key;
  }
  mutexUnlock(&g_approx_lk);
#endif
}

static void jni_approx(const char *kind, const FakeID *id) {
  if (id) jni_note_approx(kind, id->cls, id->name, id->sig);
}

/* Called from j_ExceptionCheck: on Android this is where a wrong answer would
 * have surfaced. Here it tells us the engine was LOOKING. */
static inline void jni_approx_checked(void) {
#if DFU_JNI_LEDGER
  const int idx = g_approx_last;
  if (idx < 0 || idx >= g_approx_n) return;
  if (g_approx_last_tls != (void *)&g_approx_tls_key) return;   /* other thread */
  g_approx_last = -1;
  if (!g_approx[idx].inspected) {
    g_approx[idx].inspected = 1;
    debugPrintf("[jniapx] INSPECTED  %s.%s%s -- the engine checked this call; "
                "on Android a wrong answer would have raised HERE\n",
                g_approx[idx].cls, g_approx[idx].name, g_approx[idx].sig);
  }
#endif
}

void jni_approx_summary(const char *why) {
#if !DFU_JNI_LEDGER
  (void)why;
#else
  debugPrintf("[jniapx] ===== JNI approximation ledger (%s) =====\n", why ? why : "?");
  if (!g_approx_n) { debugPrintf("[jniapx] (nothing approximated)\n"); return; }
  debugPrintf("[jniapx] %-12s %8s %5s  method\n", "kind", "hits", "insp");
  for (int i = 0; i < g_approx_n; i++)
    debugPrintf("[jniapx] %-12s %8u %5s  %s.%s%s\n",
                g_approx[i].kind, g_approx[i].hits,
                g_approx[i].inspected ? "YES" : "-",
                g_approx[i].cls, g_approx[i].name, g_approx[i].sig);
  if (g_approx_drop)
    debugPrintf("[jniapx] + %d distinct site(s) past the ledger\n", g_approx_drop);
  debugPrintf("[jniapx] INSPECTED rows are the ones the engine reads back. "
              "A wrong answer there is what becomes a bad stored value.\n");
  debugPrintf("[jniapx] ==========================================\n");
#endif
}

// ---------------------------------------------------------------------------
// local reference registry (matches the engine's Push/PopLocalFrame brackets)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    mutexUnlock(&locals_lock);
  }
  return ref;
}

// interned-string pool: the engine re-creates the same constant strings (class
// names, the activity name) constantly; pool them by content so repeats don't
// fill the local-ref table. Pooled strings are never reg_local'd, and free_ref
// skips them (range check below).
/* INTERNED STRINGS -- a growable hash pool, adopted from phigros_nx.
 *
 * This replaces a fixed `FakeString istr_pool[512]`, which had a cliff in it:
 *
 *     if (istr_count < MAX_ISTR) { ...pooled, shared, never freed... }
 *     ...
 *     FakeString *s = calloc(1, sizeof *s);   // pool full: one-off
 *     return reg_local(s);                    // FREED when the frame pops
 *
 * Past 512 distinct strings, interning silently stopped. Every string after
 * that became a frame-lifetime object, so two callers asking for the same text
 * got two different objects and each was freed when its frame popped -- while
 * anything that had kept the pointer still held it. Nothing announced the
 * changeover; the 513th string simply behaved differently from the 512th.
 *
 * Individually allocated nodes mean the pool never moves and has no limit, so
 * an interned string stays valid for the session no matter how many precede it.
 * That is also why FakeString now carries a `pooled` FLAG rather than living in
 * an address range: there is no range left to test. */
#define ISTR_BUCKETS 1024
typedef struct IStrNode { struct IStrNode *next; FakeString *s; uint32_t h; } IStrNode;
static IStrNode *istr_buckets[ISTR_BUCKETS];
static int istr_count = 0;

static uint32_t istr_hash(const char *u) {
  uint32_t h = 2166136261u;                 /* FNV-1a */
  while (*u) { h ^= (unsigned char)*u++; h *= 16777619u; }
  return h;
}

#define MAX_IOBJ 128
static FakeObject iobj_pool[MAX_IOBJ];

/* Any reference living in static storage: interned, SHARED, never freed.
 *
 * One predicate for every pool, so the next pool added cannot be missed. This
 * file previously excluded istr_pool here and relied on free_ref's `default:`
 * branch to spare iobj_pool -- which worked only because nothing cleared tags.
 * Killer Bean hit exactly that: it added a tag-clear before the free (a good
 * change on its own), and clearing the tag of a SHARED object killed it for
 * every other holder, so nx_tag_of() answered "not ours" for the rest of the
 * session and every guard silently took its wrong branch. The tag-clear below
 * is the same change, so the pool check has to be complete BEFORE it. */
/* Defined AFTER every pool, because it must test all of them and they are
 * declared throughout this file. See the definition for why the coverage
 * matters. */
static int ref_is_pooled(const void *r);

/* Retire-then-free ring.
 *
 * A use-after-free on a jobject is the failure mode this whole file is exposed
 * to: the engine keeps a reference past the PopLocalFrame that freed it, and
 * the next read faults. Guarding one call site just moves the crash to the
 * next one.
 *
 * So do not hand the struct straight back to the allocator. Retire it into a
 * ring and free it only once DFU_JNI_QUARANTINE more have been retired. The
 * struct stays mapped with tag == 0, so nx_tag_of() reports "not ours" and
 * every guard above takes its safe branch instead of faulting. The PAYLOAD
 * (utf / items / data) is freed immediately and its pointer NULLed first, so
 * nothing large is retained -- the cost is bounded by the struct sizes, the
 * largest being FakeObject at 68 bytes: 512 entries is under 35 KB.
 *
 * This does not make the use-after-free correct. The engine still reads a dead
 * reference; it now gets a clean wrong answer instead of a crash. */
#if DFU_JNI_QUARANTINE
static void   *g_retired[DFU_JNI_QUARANTINE];
static int     g_retired_w = 0;
static Mutex   g_retired_lk;
static unsigned g_retired_n = 0;

static void ref_retire(void *p) {
  mutexLock(&g_retired_lk);
  void *evict = g_retired[g_retired_w];
  g_retired[g_retired_w] = p;
  g_retired_w = (g_retired_w + 1) % DFU_JNI_QUARANTINE;
  g_retired_n++;
  mutexUnlock(&g_retired_lk);
  if (evict) free(evict);          /* outside the lock: free() takes its own */
}
unsigned nx_jni_retired(void) { return g_retired_n; }
#else
static void ref_retire(void *p) { free(p); }
unsigned nx_jni_retired(void) { return 0; }
#endif

static void free_ref(void *ref) {
  if (!ref)
    return;
  if (ref_is_pooled(ref)) return;        /* interned + shared: never freed */
  /* Clear the tag and the inner pointer BEFORE releasing either, so a racing
   * reader sees "not ours" rather than a live-looking header with a poisoned
   * payload pointer. */
  switch (nx_tag_of(ref)) {
    case TAG_STRING: { FakeString  *s = ref; char *u = s->utf;
                       s->tag = 0; s->utf = NULL;   free(u);     ref_retire(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; void *d = a->data;
                       a->tag = 0; a->data = NULL;  free(d);     ref_retire(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; void **it = a->items;
                       a->tag = 0; a->items = NULL; free(it);    ref_retire(a); break; }
    case TAG_OBJECT: { FakeObject *o = ref; o->tag = 0;          ref_retire(o); break; }
    case BITMAP_TAG: text2bitmap_free((FakeBitmap *)ref); break;
    default: break; /* TAG_ID / TAG_CLASS are pooled; 0 = not ours, already warned */
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// object constructors
// ---------------------------------------------------------------------------

// Intern objects by label -- one pooled object per class (TAG_CLASS so free_ref()
// leaves it alone, never reg_local'd) -- so the engine's frequent NewObject calls
// don't fill the local-ref table. Safe: our objects are opaque, stateless handles
// dispatched by method class, not by identity.
/* iobj_pool declared above, next to istr_pool -- see ref_is_pooled(). */
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) r = &iobj_pool[0];
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  const uint32_t h = istr_hash(u);
  IStrNode **head = &istr_buckets[h & (ISTR_BUCKETS - 1)];

  mutexLock(&locals_lock);
  for (IStrNode *n = *head; n; n = n->next)        /* repeats reuse the pooled string */
    if (n->h == h && !strcmp(n->s->utf, u)) {
      void *r = n->s; mutexUnlock(&locals_lock); return r;
    }

  FakeString *s = calloc(1, sizeof(*s));
  char *dup = strdup(u);
  IStrNode *n = calloc(1, sizeof(*n));
  if (!s || !dup || !n) { free(s); free(dup); free(n);
                          mutexUnlock(&locals_lock); return NULL; }
  s->utf    = dup;
  s->pooled = 1;                                   /* ref_is_pooled skips it */
  s->tag    = TAG_STRING;                          /* LAST: never publish half-built */
  n->s = s; n->h = h; n->next = *head; *head = n;
  istr_count++;
  { static int next_report = 4096;                 /* doubling, so it cannot spam */
    if (istr_count >= next_report) {
      debugPrintf("[jni] interned string count %d (pool is unbounded; this is "
                  "informational, not a limit)\n", istr_count);
      next_report *= 2;
    } }
  mutexUnlock(&locals_lock);
  return s;                                        /* pooled, not reg_local'd */
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (nx_tag_of(s) == TAG_STRING)
    return s->utf;
  return "";
}

// UTF-16 code-unit count of a modified-UTF-8 string (Java String.length()).
// ASCII -> byte count; astral planes count as a surrogate pair. Used by both
// GetStringLength and the String.length() upcall handler.
static juint utf16_len(const char *str) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    const unsigned char c = *p;
    juint adv; uint32_t cp;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) {
      if (!p[k]) { adv = k; break; }
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    n += (cp >= 0x10000) ? 2u : 1u;
    p += adv;
  }
  return n;
}

// register a text2bitmap result in the local table so the engine's recycle /
// DeleteLocalRef frees it
static void *reg_bitmap(FakeBitmap *b) { return reg_local(b); }

// ---------------------------------------------------------------------------
// interned classes + singletons
// ---------------------------------------------------------------------------

#define MAX_CLASSES 128
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) {
    debugPrintf("JNI: *** class pool exhausted at '%s' -> collapsing to '%s' "
                "(distinct classes break instanceof!)\n", name, class_pool[0].name);
    return &class_pool[0];
  }
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  debugPrintf("JNI class: %s\n", c->name);
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return safe_class_name(c);
}

static FakeObject *g_activity_obj = NULL;   // the MyNativeActivity instance
static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// The engine fetches the ClassLoader every frame; hand back a cached singleton
// (pooled, never reg_local'd) so it doesn't fill the local-ref table.
static FakeObject *g_classloader = NULL;
void *jni_classloader_obj(void);   /* exported wrapper, below */
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

// ---------------------------------------------------------------------------
// method/field ID pool (class-aware)
// ---------------------------------------------------------------------------

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted (%s.%s)\n", cls, name);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  debugPrintf("JNI id: %s.%s %s\n", id->cls, id->name, id->sig);
  return id;
}

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

// --- Text2Bitmap ------------------------------------------------------------
// draw methods return a Bitmap; the first arg is the text String, the next int
// is the pixel size. measure methods return I (width or height by name).

static void *t2b_object(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  FakeBitmap *b = text2bitmap_render(text, size);
  (void)id;
  return b ? reg_bitmap(b) : NULL;
}

static juint t2b_int(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  if (name_has(id->name, "Height"))
    return (juint)text2bitmap_measure_height(text, size);
  if (name_has(id->name, "Width"))
    return (juint)text2bitmap_measure_width(text, size);
  return (juint)text2bitmap_measure_width(text, size);
}

// --- MoviePlayer ------------------------------------------------------------

static const char *first_string_arg(const char *sig, va_list va); // defined below

static void mov_void(const FakeID *id, va_list va) {
  if (!strcmp(id->name, "SetMovieDB")) { movie_set_db(first_string_arg(id->sig, va)); return; }
  if (name_has(id->name, "Stop") || name_has(id->name, "stop")) { movie_stop(); return; }
  if (name_has(id->name, "Pause")) { movie_pause(); return; }
  if (name_has(id->name, "Resume")) { movie_resume(); return; }
  if (name_has(id->name, "Play") || name_has(id->name, "play") ||
      name_has(id->name, "Start")) {
    movie_play(first_string_arg(id->sig, va), 0); // the String arg is the movie name
    return;
  }
}

static juint mov_int(const FakeID *id, va_list va) {
  (void)va;
  if (name_has(id->name, "Playing") || name_has(id->name, "playing"))
    return (juint)movie_is_playing();
  return 0;
}

// --- MyNativeActivity / general activity ------------------------------------

// the in-archive base name the engine appends ".android.mvgl" to. "10007" is
// the APK versionCode, matching the shipped main.10007.android.mvgl.
#define MAIN_OBB_BASE "main.10007"

static const char *lang_code(void) {
  // English, unconditionally. PvZ Fusion ships Simplified Chinese + English, but
  // the Chinese path does not work in this port, so there is nothing to choose
  // between -- the old config.txt `language` option and the Switch
  // system-language auto-detect both went away with it. Reporting "zh" here
  // would hand the game a language it cannot render, which is why this does not
  // follow the console's own language setting.
  //
  // If the Chinese path is ever fixed, this is the single place to change: hand
  // back "zh" (or whatever token this build expects -- some want "zh-CN", see
  // PORTING.md section 5) and, if you want it selectable again, restore the
  // config parser that config.c used to hold.
  return "en";
}

// Walk a JNI arg list per the signature and return the first non-empty String
// argument's text (used to seed the keyboard from ShowEditBox's initial text).
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

// EditBox / TextBox names the engine drives via JNI (both share our swkbd box)
static int is_editbox_show(const char *n)  { return name_has(n, "ShowEditBox")  || name_has(n, "OpenEditBox")  || name_has(n, "ShowTextBox") || name_has(n, "OpenTextBox")
                                                  || name_has(n, "setKeyboardVisible") || name_has(n, "SetKeyboardVisible")
                                                  || name_has(n, "showSoftInput")      || name_has(n, "ShowSoftInput")
                                                  || name_has(n, "openKeyboard")       || name_has(n, "OpenKeyboard"); }
static int is_editbox_open(const char *n)  { return name_has(n, "IsOpenEditBox") || name_has(n, "IsOpenTextBox"); }
static int is_editbox_text(const char *n)  { return name_has(n, "GetEditBoxText") || name_has(n, "GetTextBoxText")
                                                  || name_has(n, "getKeyboardText") || name_has(n, "GetKeyboardText")
                                                  || name_has(n, "getText"); }
static int is_editbox_close(const char *n) { return name_has(n, "CloseEditBox") || name_has(n, "CloseTextBox")
                                                  || name_has(n, "hideSoftInput") || name_has(n, "HideSoftInput")
                                                  || name_has(n, "closeKeyboard") || name_has(n, "CloseKeyboard"); }
/* Anything keyboard-shaped that we did NOT match: log once so the exact JNI name
 * this game uses is visible and can be added above. */
static void kbd_sniff(const char *cls, const char *n) {
  if (!name_has(n, "eyboard") && !name_has(n, "oftInput") && !name_has(n, "extBox")
      && !name_has(n, "ditBox") && !name_has(cls, "eyboard")) return;
  static unsigned seen; if (seen < 12) { seen++;
    debugPrintf("[kbd] unmatched: %s.%s\n", cls ? cls : "?", n ? n : "?"); }
}

/* jni_string_utf is defined far below (shared with unity_jni.c) and unity_jni.h
 * is included after this point, so forward-declare it for act_object's
 * getProperty()/locale arg reads. */
const char *jni_string_utf(void *jstr);

/* AudioManager.getProperty(PROPERTY_OUTPUT_*): the engine/FMOD read the native
 * sample rate / frames-per-buffer to size the audio path. Empty -> parse failure
 * -> a 0 config, which makes FMOD's OpenSL output init fail with "Error
 * initializing output device" (60) on the framesPerBuffer==0 guard. Hand back
 * Switch-sane values (48 kHz, 64 frames).
 *
 * The String key argument does NOT reliably reach us: getProperty is invoked
 * through a JNI call path whose positional argument is lost (observed: key
 * resolves to ""), so keying purely off the argument returned "" and FMOD parsed
 * framesPerBuffer 0 -> error 60. The game ALWAYS reads the PROPERTY_OUTPUT_*
 * static field immediately before the matching getProperty() call, so field_object
 * records which one in g_last_output_prop and we fall back to it when the key is
 * missing/unrecognised. 1 = sample rate, 2 = frames-per-buffer. */
static int g_last_output_prop = 0;

static void *getproperty_value(const char *key) {
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  static int logged[3] = {0, 0, 0};
  if (which >= 0 && which <= 2 && !logged[which]) {
    logged[which] = 1;
    debugPrintf("[jni] getProperty -> %s\n",
                which == 1 ? "24000" : which == 2 ? "256" : "(empty)");
  }
  if (which == 1) return jni_make_string("24000");
  if (which == 2) return jni_make_string("256");
  return jni_make_string("");
}

extern void *fake_env;
typedef void *(*jnibridge_invoke_fn)(void *, void *, long long, void *, void *, void *);
static jnibridge_invoke_fn g_jnibridge_invoke = 0;
static void *j_NewObjectArray(void *env, int len, void *cls, void *init);
/* jni_make_object pools by label, which would collapse every proxy to one object.
 * Give each proxy its own object that embeds its native ptr; identify proxies by
 * address range (safe -- never reads a field on a non-proxy jobject). */
#define MAX_PROXY_OBJ 512
typedef struct { uint32_t tag; uint32_t pad; long long ptr; } FakeProxy;
static FakeProxy g_proxy_pool[MAX_PROXY_OBJ];

/* Is this reference one of the SHARED, never-freed pool entries?
 *
 * Pooled references are handed to every caller that asks for the same class,
 * method id, proxy or interned string. Freeing one -- or merely clearing its
 * tag -- kills it for every other holder: from then on nx_tag_of() reports
 * "not ours" for that reference for the rest of the session, and every guard
 * downstream silently takes its wrong branch. Nothing crashes at the point of
 * damage; it surfaces later somewhere unrelated.
 *
 * This test must come FIRST in free_ref() and cover EVERY pool. Previously it
 * covered only iobj_pool (and, since sec 26, interned strings). class_pool,
 * id_pool and g_proxy_pool survived purely because their TAG_CLASS / TAG_ID
 * tags happen to fall through free_ref()'s switch to `default: break`.
 *
 * That is luck, not design, and it stops being true the moment anything clears
 * a tag before freeing -- which this file already does in four places. The
 * observation, and the wording, are from phigros_nx, which had it right.
 *
 * The three JNI singletons are checked explicitly: they are individually
 * allocated rather than living in any array, so no range test can reach them. */
static int ref_is_pooled(const void *r) {
  const char *p = (const char *)r;
  if (!p) return 0;
  if (r == (const void *)g_activity_obj ||
      r == (const void *)g_asset_mgr    ||
      r == (const void *)g_classloader) return 1;
  /* Interned strings are individually allocated (sec 26), so there is no range
   * to test -- they carry a flag. Checked early because it is the common case. */
  if (nx_tag_of(r) == TAG_STRING && ((const FakeString *)r)->pooled) return 1;
  return (p >= (const char *)iobj_pool     && p < (const char *)&iobj_pool[MAX_IOBJ])     ||
         (p >= (const char *)class_pool    && p < (const char *)&class_pool[MAX_CLASSES]) ||
         (p >= (const char *)id_pool       && p < (const char *)&id_pool[MAX_IDS])        ||
         (p >= (const char *)g_proxy_pool  && p < (const char *)&g_proxy_pool[MAX_PROXY_OBJ]);
}
static int g_proxy_pool_n = 0;
static FakeProxy *g_last_proxy = 0;
static void *proxy_make(long long ptr) {
  if (g_proxy_pool_n >= MAX_PROXY_OBJ) return jni_make_object("jniproxy");
  FakeProxy *p = &g_proxy_pool[g_proxy_pool_n++];
  p->tag = TAG_CLASS; p->ptr = ptr; g_last_proxy = p;   /* TAG_CLASS -> free_ref ignores it */
  return p;
}
static long long proxy_ptr_of(void *obj) {
  uintptr_t a = (uintptr_t)obj, lo = (uintptr_t)g_proxy_pool, hi = (uintptr_t)(g_proxy_pool + g_proxy_pool_n);
  if (a >= lo && a < hi && ((a - lo) % sizeof(FakeProxy)) == 0) return ((FakeProxy *)obj)->ptr;
  return 0;
}
/* Invoke one method on a proxy through Unity's bridge. ProxyObject dispatch checks
   the jclass matches the interface and the methodID == the method (pointer compare);
   j_FromReflectedMethod passes a TAG_ID methodID straight through so it matches. */
static void proxy_invoke(void *proxy, void *cls, void *method, void *args) {
  long long ptr = proxy_ptr_of(proxy);
  if (!ptr || !g_jnibridge_invoke) return;
  {
    /* doFrame arrives 60 times a second from the vsync pump. Log the first
     * few, then one in 600 (~every 10 s) so the log shows the pump is alive
     * without drowning everything else (boot 28: 110 of 119 lines/s). */
    const char *mn = ((FakeID *)method)->name;
    static unsigned nframe = 0;
    int quiet = !strcmp(mn, "doFrame") && (nframe++ > 5) && (nframe % 600u != 0);
    if (!quiet) debugPrintf("[jni] proxy invoke %p ptr=%llx .%s%s\n", proxy, (unsigned long long)ptr, mn,
                            !strcmp(mn, "doFrame") && nframe > 5 ? " (1 in 600 logged)" : "");
  }
  g_jnibridge_invoke(fake_env, proxy, ptr, cls, method, args);
}
static void proxy_run_runnable(void *runnable) {
  proxy_invoke(runnable, intern_class("java/lang/Runnable"), get_id("java/lang/Runnable", "run", "()V"), (void *)0);
}
/* Message.sendToTarget(): the factory's HandlerThread never pumps its Looper, so the
   message is never delivered to callback.handleMessage(msg). Deliver it ourselves to
   the most-recently created proxy (the Handler$Callback the factory just built). */
static void handler_deliver_message(void *callback) {
  void *msg  = jni_make_object("android/os/Message");
  void *args = j_NewObjectArray(fake_env, 1, (void *)0, msg);
  proxy_invoke(callback, intern_class("android/os/Handler$Callback"),
               get_id("android/os/Handler$Callback", "handleMessage", "(Landroid/os/Message;)Z"), args);
}
static uint64_t g_frame_ns = 0;   /* frameTimeNanos for boxed-Long unboxing */
static void *g_frame_cb = 0;      /* registered Choreographer FrameCallback proxy */
static void deliver_doframe(void *cb) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  g_frame_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  void *boxed = jni_make_object("java/lang/Long");
  void *args  = j_NewObjectArray(fake_env, 1, (void *)0, boxed);
  static int logged = 0;
  if (logged < 3) { logged++; debugPrintf("[jni] doFrame tick -> %p (vsync pump)\n", cb); }
  proxy_invoke(cb, intern_class("android/view/Choreographer$FrameCallback"),
               get_id("android/view/Choreographer$FrameCallback", "doFrame", "(J)V"), args);
}
/* drain thread: runs posted work off the main thread (no self-deadlock / re-entrancy) */
#define RUNQ_N 128
static void *g_runq[RUNQ_N]; static int g_runq_kind[RUNQ_N]; static int g_runq_head = 0, g_runq_tail = 0;
static Mutex g_runq_lk; static CondVar g_runq_cv; static int g_runq_started = 0;
static void run_drain_thread(void *arg) {
  (void)arg;
  static uint8_t drain_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(drain_tls);
  for (;;) {
    mutexLock(&g_runq_lk);
    if (g_runq_head == g_runq_tail) {
      if (g_frame_cb) condvarWaitTimeout(&g_runq_cv, &g_runq_lk, 16000000ULL); /* vsync tick */
      else            condvarWait(&g_runq_cv, &g_runq_lk);
    }
    void *o = 0; int k = 0, have = 0;
    if (g_runq_head != g_runq_tail) {
      o = g_runq[g_runq_head]; k = g_runq_kind[g_runq_head];
      g_runq_head = (g_runq_head + 1) % RUNQ_N; have = 1;
    }
    void *fcb = g_frame_cb; g_frame_cb = 0;   /* one-shot: doFrame re-registers */
    mutexUnlock(&g_runq_lk);
    if (have) { if (k == 0) proxy_run_runnable(o); else handler_deliver_message(o); }
    if (fcb) deliver_doframe(fcb);
  }
}
static Thread g_runq_thr;
static void runq_post(void *obj, int kind) {
  if (!obj) return;
  mutexLock(&g_runq_lk);
  if (!g_runq_started) {
    g_runq_started = 1;
    if (R_SUCCEEDED(threadCreate(&g_runq_thr, run_drain_thread, NULL, NULL, 0x8000, 0x2C, -2)))
      threadStart(&g_runq_thr);
  }
  int nt = (g_runq_tail + 1) % RUNQ_N;
  if (nt != g_runq_head) { g_runq[g_runq_tail] = obj; g_runq_kind[g_runq_tail] = kind; g_runq_tail = nt; }
  condvarWakeOne(&g_runq_cv);
  mutexUnlock(&g_runq_lk);
}
static void post_runnable(void *runnable) { runq_post(runnable, 0); }
static void post_message(void) { runq_post(g_last_proxy, 1); }
static int g_msg_what = 0;   /* captured from Handler.obtainMessage(what) for msg.what reads */
unsigned g_kbd_trace;      /* set when swkbd closes; counts down as we log */
/* Unity soft-input natives (captured at RegisterNatives, invoked on close). */
void *g_u_setInputString, *g_u_setInputSel, *g_u_softClosed,
     *g_u_softCancel, *g_u_kbdVisible;
/* Push the swkbd result into Unity exactly as its Java keyboard would. */
void kbd_push_result(const char *text, int cancelled) {
  typedef void (*fn_str)(void *, void *, void *);
  typedef void (*fn_ii)(void *, void *, int, int);
  typedef void (*fn_v)(void *, void *);
  typedef void (*fn_z)(void *, void *, int);
  void *cls = intern_class("com/unity3d/player/UnityPlayer");
  if (!text) text = "";
  int len = (int)strlen(text);
  debugPrintf("[kbd] push \"%s\" cancelled=%d (str=%p closed=%p)\n",
              text, cancelled, g_u_setInputString, g_u_softClosed);
  if (!cancelled && g_u_setInputString)
    ((fn_str)g_u_setInputString)(fake_env, cls, jni_make_string(text));
  if (!cancelled && g_u_setInputSel)
    ((fn_ii)g_u_setInputSel)(fake_env, cls, len, len);
  if (g_u_kbdVisible) ((fn_z)g_u_kbdVisible)(fake_env, cls, 0);
  if (cancelled) { if (g_u_softCancel) ((fn_v)g_u_softCancel)(fake_env, cls); }
  else           { if (g_u_softClosed) ((fn_v)g_u_softClosed)(fake_env, cls); }
}
static void *act_object(const FakeID *id, va_list va) {
  if (g_kbd_trace) { g_kbd_trace--;
    debugPrintf("[kbd] after-kbd obj call: %s.%s sig=%s\n",
                id->cls, id->name,
                id->sig); }
  /* A String-returning keyboard call: show it, then hand back what was typed.
   * This is the path this game uses -- it never polls a getter afterwards. */
  if (is_editbox_show(id->name) && sig_returns(id->sig, "Ljava/lang/String;")) {
    editbox_show(first_string_arg(id->sig, va), 64);
    const char *t = editbox_text();
    debugPrintf("[kbd] show-returns-text %s.%s -> \"%s\"\n",
                id->cls, id->name, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  if (is_editbox_text(id->name)) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text getter(obj) %s.%s -> \"%s\"\n",
                id->cls, id->name, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  /* JNIBridge.newInterfaceProxy(long nativePtr, Class[] ifaces) -> remember the
   * proxy so a later Handler.post(runnable) can invoke its run() (see post_runnable). */
  if (name_has(id->name, "newInterfaceProxy")) {
    long long ptr = va_arg(va, long long);
    void *proxy = proxy_make(ptr);
    debugPrintf("[jni] newInterfaceProxy ptr=%llx -> proxy=%p\n", (unsigned long long)ptr, proxy);
    return proxy;
  }
  /* Handler.obtainMessage(what): must return a REAL Message or the game's C++
   * wrapper null-checks and silently skips msg.sendToTarget() -- the UI-manager
   * factory then waits forever for handleMessage's side effect. Capture 'what'
   * so the callback's msg.what field read sees the right value. */
  if (name_has(id->name, "obtainMessage")) {
    if (id->sig[1] == 'I') g_msg_what = va_arg(va, int);
    debugPrintf("[jni] obtainMessage what=%d -> Message\n", g_msg_what);
    return jni_make_object("android/os/Message");
  }
  if (name_has(id->name, "getLooper") || name_has(id->name, "getMainLooper"))
    return jni_make_object("android/os/Looper");
  if (name_has(id->name, "getInstance") && name_has(id->cls, "Choreographer"))
    return jni_make_object("android/view/Choreographer");
  /* Unity launch args: libunity reads currentActivity.getIntent().getStringExtra("unity").
   * Serve -job-worker-count 0 to run jobs inline on main: diagnostic for the
   * UIGeometryJob garbage-input crash (if it persists inline, the corruption
   * predates the job and the crash stack shows the producer; if it vanishes,
   * it's a job/fence lifetime race) -- and a potential playable workaround.
   * The JobSystem log line 'Creating JobQueue using job-worker-count value %d'
   * confirms the effective value. */
  if (name_has(id->name, "getIntent"))
    return jni_make_object("android/content/Intent");
  if (name_has(id->name, "getExtras"))
    return jni_make_object("android/os/Bundle");
  if (name_has(id->cls, "Bundle") && (name_has(id->name, "getString") || name_has(id->name, "getCharSequence"))) {
    debugPrintf("[jni] Bundle.%s -> launch args served\n", id->name);
    return jni_make_string("");
  }
  if (name_has(id->name, "getStringExtra")) {
    const char *k = first_string_arg(id->sig, va);
    if (k && !strcmp(k, "unity")) {
      debugPrintf("[jni] getStringExtra(unity) -> launch args served\n");
      return jni_make_string("");
    }
    return NULL;
  }
  /* Uri.encode/decode: Unity round-trips PlayerPrefs keys through these. We are
   * the storage, so identity (return the input string) round-trips correctly and
   * keeps keys non-empty. Must precede the generic handlers. */
  if (name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return va_arg(va, void *);
  /* android.hardware.SensorManager: there is no accelerometer/gyro exposed
   * through this path (libnx six-axis is not wired to Android's SensorManager
   * here). getDefaultSensor(type) returning an OPAQUE object told Rewired a
   * sensor existed; it then called methods on it and got garbage. null is the
   * Android answer for "no such sensor" and every caller checks for it. */
  if (name_has(id->cls, "hardware/SensorManager")) {
    if (name_has(id->name, "getDefaultSensor")) return NULL;
    if (name_has(id->name, "getSensorList") || name_has(id->name, "getDynamicSensorList"))
      return jni_make_object("java/util/ArrayList");   /* empty; size() -> 0 */
  }
  /* android.media.midi.MidiManager, as used by libRtMidi's Android backend
   * now that it has a JavaVM. No MIDI transport exists on this platform (see
   * ch_midi.c), so the honest answer to every enumeration is an EMPTY array --
   * not an opaque object, which RtMidi would hand to GetArrayLength and then
   * index. Callbacks are registered into the void; nothing will ever fire. */
  if (name_has(id->name, "getDevices") || name_has(id->name, "getDevicesForTransport"))
    return jni_make_object_array(0);
  if (name_has(id->cls, "midi/MidiManager") && name_has(id->name, "openDevice"))
    return NULL;                                     /* never called with 0 devices */
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  // version / package / device / storage strings
  if (name_has(id->name, "VersionName")) return jni_make_string("2.1.6");
  if (name_has(id->name, "PackageName")) return jni_make_string("jp.kiteretsu.zookeeper_dx");
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  // archive name getters: the engine builds "<dir>/<name>.android.mvgl" for 5
  // slots (main + patch + 3 asset packs). We map them to the 5 shipped archives
  // (main.10007 + the four CRDB media DBs) so all of them mount.
  if (name_has(id->name, "ObbMainFileName"))  return jni_make_string(MAIN_OBB_BASE);
  if (name_has(id->name, "ObbPatchFileName")) return jni_make_string("CRDBbgm");
  if (name_has(id->name, "AssetPack1"))       return jni_make_string("CRDBvoice");
  if (name_has(id->name, "AssetPack2"))       return jni_make_string("CRDBse");
  if (name_has(id->name, "AssetPack3"))       return jni_make_string("CRDBmov");
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  /* ---- top approximations from the FMV-stall run's ledger ---------------- *
   * These were the four busiest OBJ-guess rows, all INSPECTED (the engine
   * ExceptionCheck'd them, so a wrong answer becomes a stored value):
   *     103  Settings$Secure.getString
   *      17  AudioManager.getDevices
   *       6  MediaRouter.getSelectedRoute
   *       4  Display.getSupportedModes          (OPAQUE-OBJ)
   * An opaque guess is the worst answer available for all four: the caller
   * reads fields off it and gets whatever the guess happens to contain. */

  /* Settings.Secure / Settings.Global getString(resolver, key). Unity asks for
   * android_id and a handful of device flags. A guessed object 103 times is
   * garbage 103 times; a real string is cheap and honest. Unknown keys return
   * "" rather than null, because null here is what makes callers NPE. */
  if (name_has(id->cls, "provider/Settings") && name_has(id->name, "getString")) {
    va_arg(va, void *);                                  /* ContentResolver */
    const char *key = jni_string_utf(va_arg(va, void *));
    if (key && !strcmp(key, "android_id"))
      return jni_make_string("53574954434800aa");         /* stable, per-install */
    return jni_make_string("");
  }

  /* AudioManager.getDevices() -> AudioDeviceInfo[]. An EMPTY array is a true answer
   * on a console with no Android audio devices to enumerate, and Unity handles
   * it (it just finds no routing candidates and keeps the default output, which
   * is the FMOD/OpenSL sink this port already drives). An opaque guess makes it
   * iterate a non-array. */
  if (name_has(id->cls, "media/AudioManager") && name_has(id->name, "getDevices"))
    return j_NewObjectArray(NULL, 0, NULL, NULL);

  /* Display.getSupportedModes() -> Display.Mode[]. Same reasoning, and this one
   * has a visible symptom: the log shows AndroidScreenManager flipping between
   * 1280x720 and 1920x1080 over and over, which is what happens when the mode
   * list it is choosing from is not a mode list. Empty makes Unity keep the
   * mode it already has. */
  if (name_has(id->cls, "view/Display") && name_has(id->name, "getSupportedModes"))
    return j_NewObjectArray(NULL, 0, NULL, NULL);

  /* MediaRouter.getSelectedRoute(type) -> RouteInfo. There is no media router;
   * NULL is the documented "no route" answer and callers null-check it, whereas
   * an opaque object gets its name/volume read. */
  if (name_has(id->cls, "media/MediaRouter") && name_has(id->name, "getSelectedRoute"))
    return NULL;

  // Environment.getExternalStorageState() must return the SAME token as the
  // Environment.MEDIA_MOUNTED field ("mounted", see field_object) or the engine
  // decides external storage is unavailable and the save path never initialises.
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  // Locale.getCountry/getISO3*/toString/getDisplayName: the game reads the
  // default locale (UnityPlayer.getDefault) for its language pick; "" here left
  // the locale blank in the log. Mirror lang_code()'s ja/en choice. Guarded by
  // the Locale class so we don't hijack toString()/getCountry on other objects.
  if (name_has(id->cls, "Locale")) {
    /* English only, to match lang_code(). Reporting a Chinese locale here would
     * contradict the language the game is actually being handed. */
    if (!strcmp(id->name, "getCountry"))     return jni_make_string("US");
    /* getLanguage was missing: Rewired's Android init does
     * Locale.getDefault().getLanguage() and the miss fell to a generic answer.
     * Boot 20's NRE was logged immediately after this exact call sequence. */
    if (!strcmp(id->name, "getLanguage"))    return jni_make_string("en");
    if (!strcmp(id->name, "getVariant") || !strcmp(id->name, "getScript")) return jni_make_string("");
    if (name_has(id->name, "getDisplayCountry")) return jni_make_string("United States");
    if (!strcmp(id->name, "toLanguageTag")) return jni_make_string("en-US");
    if (!strcmp(id->name, "getISO3Language"))return jni_make_string("eng");
    if (!strcmp(id->name, "getISO3Country")) return jni_make_string("USA");
    if (!strcmp(id->name, "toString") || name_has(id->name, "getDisplayName") ||
        name_has(id->name, "getDisplayLanguage"))
      return jni_make_string("en_US");
  }
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path"))
    return jni_make_string(managed_path(data_dir()));
  // text the user typed on the Switch software keyboard
  if (is_editbox_text(id->name)) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text getter %s.%s -> \"%s\"\n",
                id->cls, id->name, t ? t : "(null)");
    return jni_make_string(t);
  }
  kbd_sniff(id->cls, id->name);   /* a jstring getter lands here, not in act_void */
  // asset-pack names ("" is fine: the engine appends the hardcoded CRDB* name,
  // and the data layer's basename fallback finds the flat file regardless)
  // Android object getters that must NOT be null, or the engine aborts the
  // chain. getPackageInfo()/getApplicationInfo() are how Unity reaches
  // PackageInfo.versionName/versionCode (-> Application.version); returning null
  // here is why the version stayed blank even with field access fixed -- Unity
  // got a null PackageInfo and never read the field. Hand back live (opaque)
  // objects; the subsequent field reads then resolve via field_object/field_int.
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  if (sig_returns(id->sig, "Ljava/lang/String;"))
    return jni_make_string(""); // UUID, asset-pack name, etc.
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  if (name_has(id->cls, "Handler") && name_has(id->name, "post")) {   /* post/postDelayed(Runnable) */
    post_runnable(va_arg(va, void *)); return 1;
  }
  /* Bundle.containsKey(key): the launch-args probe -- true for the 'unity' extra so
   * Unity proceeds to Bundle.getString(unity) (see act_object) and applies args. */
  if (name_has(id->cls, "Bundle") && name_has(id->name, "containsKey")) {
    const char *k = first_string_arg(id->sig, va);
    /* PVZ_UNITY_LAUNCH_ARGS is empty now: the -job-worker-count 0 injection was a
     * crash-era experiment. Starving the job workers stops Unity building
     * culling/batching/UI geometry -> frames advance but nothing renders.
     * Report the extra as ABSENT so Unity uses its normal defaults. */
    int hit = 0; (void)k;
    debugPrintf("[jni] Bundle.containsKey(%s) -> %d\n", k ? k : "?", hit);
    return hit ? 1 : 0;
  }
  // java.lang.Integer.parseInt(String[,radix]) / Long.parseLong: FMOD's audio
  // path parses getProperty()'s "48000"/"64" results through these. The old
  // act_int fall-through returned 0 -> framesPerBuffer parsed to 0 -> FMOD's
  // OpenSL output init failed with "Error initializing output device" (60).
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u\n", id->name, s ? s : "", v);
    return v;
  }
  if (is_editbox_open(id->name)) return (juint)editbox_is_open();
  // some builds expose Show/Open as an int (success) call rather than void
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 32); return 1; }
  // Play Asset Delivery: with NO Play Core on Switch, the engine MUST take the
  // "missing" path, where it treats every asset pack as install-time/local and
  // reads assets synchronously from the APK/bundle. Returning false (the old
  // default) tells Unity Play Core IS present, so it uses the ASYNC AssetPackManager
  // path -- it calls getAssetPackState() with a callback that we never invoke and
  // then waits forever for the pack, so the resident scene never loads and
  // ResidentSystem.Awake never runs (the live boot stall). Return true.
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  // The other Google check whose default (0 == ConnectionResult.SUCCESS) wrongly
  // means "Play Services available": isGooglePlayServicesAvailable(). Return
  // SERVICE_MISSING (1) so the Play Games plugin cleanly disables itself instead
  // of trying to sign in against GMS that isn't there. (Not hit during boot --
  // Play Games activates on user action -- but correct for when it is.)
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  /* Storage permission. The log shows the game calling
   *   Context.checkCallingOrSelfPermission(Ljava/lang/String;)I
   * on its first-run path, right where Android would put up the WRITE_EXTERNAL
   * _STORAGE dialog. PackageManager.PERMISSION_GRANTED is 0 and act_int's
   * default is also 0, so this happened to work -- but only by coincidence, and
   * a coincidence is a bad thing to rest a boot path on. State it.
   *
   * Granted is the honest answer: we are not sandboxed, the folder is ours, and
   * every file operation the game attempts will in fact succeed. */
  if (name_has(id->name, "checkCallingOrSelfPermission") ||
      name_has(id->name, "checkSelfPermission") ||
      name_has(id->name, "checkPermission"))
    return 0;   /* PackageManager.PERMISSION_GRANTED */
  /* Scoped-storage probes (API 30+). We have full access to our own folder, so
   * both are true; answering false sends the game down a SAF/document-picker
   * path that has no UI here. */
  if (name_has(id->name, "isExternalStorageManager") ||
      name_has(id->name, "isExternalStorageLegacy") ||
      name_has(id->name, "isExternalStorageEmulated")) return 1;

  (void)va;
  // every other "is something open / clicked / ok" probe -> false/0
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  return 0.0f;
}

static void act_void(const FakeID *id, va_list va) {
  if (name_has(id->name, "runOnUiThread")) { post_runnable(va_arg(va, void *)); return; }
  if (name_has(id->name, "sendToTarget")) { post_message(); return; }
  if (name_has(id->name, "postFrameCallback")) {
    void *cb = va_arg(va, void *);
    mutexLock(&g_runq_lk);
    if (!g_runq_started) {
      g_runq_started = 1;
      if (R_SUCCEEDED(threadCreate(&g_runq_thr, run_drain_thread, NULL, NULL, 0x8000, 0x2C, -2)))
        threadStart(&g_runq_thr);
    }
    g_frame_cb = cb;
    condvarWakeOne(&g_runq_cv);
    mutexUnlock(&g_runq_lk);
    static int fclog = 0; if (fclog < 3) { fclog++; debugPrintf("[jni] postFrameCallback cb=%p (vsync pump on)\n", cb); }
    return;
  }
  if (name_has(id->name, "removeFrameCallback")) {
    mutexLock(&g_runq_lk); g_frame_cb = 0; mutexUnlock(&g_runq_lk); return;
  }
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 64); return; }
  if (is_editbox_close(id->name)) { editbox_close(); return; }
  kbd_sniff(id->cls, id->name);
  (void)va;
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
  // openStore / sendBroadcast / IME open / Mobage / web view: no-op
}

// ---------------------------------------------------------------------------
// top-level dispatch by class + return kind
// ---------------------------------------------------------------------------

/* ZOOKEEPER DX port: delegate Unity/input classes to our modules */
#include "unity_jni.h"
#include "unity_input.h"

static int is_t2b(const char *cls)  { return name_has(cls, "Text2Bitmap"); }
static int is_mov(const char *cls)  { return name_has(cls, "MoviePlayer"); }

// Breadcrumb: the game's own Java side (jp.kiteretsu.* save/load + license
// plugin) is reached only through JNI upcalls. The DEX shows the exact classes
// (loadsavedata.{SRecord,SCryption,SUtility,NativeLoad}, LicenseVerification),
// but not which the C# actually invokes or in what order. Log each unique
// app-class upcall once so the first run that reaches the save/license stage
// tells us precisely what to implement, instead of guessing. Behaviour is
// unchanged: after logging, the call still falls through to the act_* handlers.
static void log_app_upcall(const FakeID *id) {
  if (!name_has(id->cls, "kiteretsu")) return;
  static const char *seen[64]; static int seen_n = 0;
  for (int i = 0; i < seen_n; i++) if (seen[i] == id->name) return; // interned name ptr
  if (seen_n < 64) seen[seen_n++] = id->name;
  debugPrintf("[jni] app upcall: %s.%s%s\n", id->cls, id->name, id->sig);
}

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  jni_approx_arm();
  log_app_upcall(id);

  /* ======================================================================
   * com/unity3d/player/ReflectionHelper -- THE GATEWAY FOR THE GAME'S OWN CODE
   *
   * Every AndroidJavaObject.Call / Get / CallStatic / GetStatic in C# resolves
   * its target through these, not through GetMethodID directly. Unity's
   * AndroidReflection asks ReflectionHelper.getMethodID / getFieldID for a
   * java.lang.reflect.Method / Field, then converts it with FromReflected*.
   *
   * None of this was handled. The chain that stalled Clone Hero:
   *   getFieldID(Environment, "DIRECTORY_DOCUMENTS", sig, true) -> opaque obj
   *   getFieldSignature(opaque)                                  -> garbage
   *   Unity falls back to GetStaticFieldID(cls, name, garbage="")
   *   field_object: no DIRECTORY_DOCUMENTS case, sig "" matches nothing -> NULL
   *   C#: GetStatic<string> returns null -> NullReferenceException in Awake
   *   -> the startup coroutine dies -> loading screen forever.
   *
   * The fix is small because the pieces already exist: return OUR FakeID
   * (TAG_ID) as the "reflected" object. FromReflectedMethod/Field then pass it
   * straight through (they already do for TAG_ID), and get*Signature can read
   * the sig back off it. No Field/Method object model needed.
   *
   * Args: (Class cls, String name, String sig, boolean isStatic). cls is our
   * FakeClass; safe_class_name() reads it without trusting the pointer.
   * ==================================================================== */
  if (name_has(id->cls, "unity3d/player/ReflectionHelper")) {
    if (!strcmp(id->name, "getMethodID") || !strcmp(id->name, "getFieldID")) {
      void *cls  = va_arg(va, void *);
      void *name = va_arg(va, void *);
      void *sig  = va_arg(va, void *);
      const char *cn = safe_class_name(cls);
      const char *nm = safe_utf(name);
      const char *sg = safe_utf(sig);
      if (!*cn) cn = "java/lang/Object";
      return get_id(cn, nm, sg);              /* TAG_ID -- the reflected object */
    }
    if (!strcmp(id->name, "getConstructorID")) {
      void *cls = va_arg(va, void *);
      void *sig = va_arg(va, void *);
      const char *cn = safe_class_name(cls);
      return get_id(*cn ? cn : "java/lang/Object", "<init>", safe_utf(sig));
    }
    if (!strcmp(id->name, "getFieldSignature") || !strcmp(id->name, "getMethodSignature")) {
      void *f = va_arg(va, void *);
      if (nx_tag_of(f) == TAG_ID) {
        const FakeID *fid = f;
        /* If the sig came in empty (Unity asked by name only), infer it: a
         * SCREAMING_CASE name on a String-typed class constant is a String. */
        if (fid->sig[0]) return jni_make_string(fid->sig);
        int caps = 1;
        for (const char *c = fid->name; *c; c++)
          if (!((*c >= 'A' && *c <= 'Z') || *c == '_' || (*c >= '0' && *c <= '9'))) { caps = 0; break; }
        return jni_make_string(caps ? "Ljava/lang/String;" : "()V");
      }
      return jni_make_string("()V");
    }
    /* newProxyInstance / createInvocationError are handled by the proxy
     * machinery further down; anything else here is benign. */
  }

  /* java.lang.reflect.Field / Method / Constructor methods called ON one of
   * the FakeIDs we hand back as "reflected" objects. Unity's AndroidReflection
   * calls field.getDeclaringClass() right after getFieldID (it was the last id
   * resolved before the crash) and uses the result as the class for the
   * subsequent Get/SetStatic*Field. Answer with the class the id belongs to. */
  if (nx_tag_of(recv) == TAG_ID) {
    const FakeID *rid = recv;
    if (!strcmp(id->name, "getDeclaringClass") || !strcmp(id->name, "getType") ||
        !strcmp(id->name, "getReturnType"))
      return intern_class(rid->cls);
    if (!strcmp(id->name, "getName")) return jni_make_string(rid->name);
    if (!strcmp(id->name, "toString")) {
      char buf[320]; snprintf(buf, sizeof buf, "%s.%s%s", rid->cls, rid->name, rid->sig);
      return jni_make_string(buf);
    }
    if (!strcmp(id->name, "getParameterTypes") || !strcmp(id->name, "getExceptionTypes") ||
        !strcmp(id->name, "getDeclaredAnnotations") || !strcmp(id->name, "getAnnotations"))
      return jni_make_object_array(0);
  }

  /* MotionEvent.obtain(MotionEvent): copy factory. The engine copies our
   * injected event and reads the copy after inject returns; return a real
   * UEvent copy so getSource/getX/getY on it hit our handlers (else they read
   * 0, getSource looks non-touch, and the event is dropped before getX/getY). */
  if (input_owns_class(id->cls) && !strcmp(id->name, "obtain") &&
      strstr(id->sig, "(Landroid/view/MotionEvent;)")) {
    void *src = va_arg(va, void *);
    return unity_motionevent_obtain(src);
  }
  /* String.getBytes([charset]) -> byte[] of the string's UTF-8 bytes. Unity's
   * PlayerPrefs key-encoding is key.getBytes() -> new String([B,charset) ->
   * Uri.encode(...); without real bytes the whole chain collapsed to "" and
   * every encoded-key pref collided. Route by the FakeString receiver. */
  if (nx_tag_of(recv) == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = safe_utf(recv); int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  if (unity_owns_class(id->cls) || unity_owns_recv(recv)) return unity_dispatch_object(recv, id, va);
  /* InputDevice / MotionRange / List handles from unity_input.c (Rewired's
   * controller enumeration). Static getDevice/getDeviceIds arrive with the
   * class as receiver, so match the class too. */
  if ((input_owns_class(id->cls) && (name_has(id->name, "getDevice") || input_owns_recv(recv))) ||
      input_owns_recv(recv)) {
    void *r = input_dispatch_object(recv, id, va);
    if (r || input_owns_recv(recv) || name_has(id->name, "getDevice")) return r;
  }
  // any method returning a Bitmap is text rendering (Char2Bitmap / getShadowBitmap
  // / ...): the loaded class always reads back as java/lang/Object, so route by
  // return type rather than class name.
  const int wants_bitmap = sig_returns(id->sig, "Landroid/graphics/Bitmap;");
  /* Uri.encode/decode are answered EXACTLY (identity) by act_object; noting
   * them as a guess here made the ledger report "INSPECTED Uri.decode" on
   * every PlayerPrefs read and sent boot 16's investigation the wrong way. */
  const int exact = name_has(id->cls, "net/Uri") &&
                    (name_has(id->name, "encode") || name_has(id->name, "decode"));
  if (!is_t2b(id->cls) && !wants_bitmap && !exact)
    jni_approx("OBJ-guess", id);   /* act_object invents a plausible object */
  return (is_t2b(id->cls) || wants_bitmap) ? t2b_object(id, va) : act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  jni_approx_arm();
  log_app_upcall(id);
  // java.lang.String instance methods reached via CallIntMethod (Unity's
  // java::lang::String::length() does this to size path buffers). The receiver
  // is our FakeString; GetObjectClass reports it as java/lang/Object, so route
  // on the receiver tag + method name, NOT id->cls. Returning 0 here (the old
  // act_int fall-through) undersizes the OBB-path sprintf buffer and overflows.
  if (nx_tag_of(recv) == TAG_STRING) {
    const char *s = safe_utf(recv);
    if (!strcmp(id->name, "length"))   return utf16_len(s);
    if (!strcmp(id->name, "isEmpty"))  return s[0] == '\0';

    /* equals / equalsIgnoreCase / compareTo / startsWith / endsWith / contains.
     *
     * THIS IS LOAD-BEARING. Clone Hero's first-run path does
     *
     *     Environment.getExternalStorageState().equals(Environment.MEDIA_MOUNTED)
     *
     * and both sides are already correct here -- getExternalStorageState()
     * returns "mounted" (act_object) and the MEDIA_MOUNTED field returns
     * "mounted" (field_object). But CallBooleanMethod routes to dispatch_int,
     * and with no "equals" case the call fell through to act_int, whose default
     * is 0. The game therefore concluded external storage was NOT mounted,
     * skipped its unpack/extract step entirely, and sat on the loading screen
     * forever -- rendering frames the whole time, which is why it looked alive.
     *
     * The approximation ledger caught this: getExternalStorageState() came back
     * INSPECTED, meaning the engine read our answer straight after asking. It
     * was the right answer; the comparison was what broke.
     *
     * The argument is a jobject, so validate it as a string rather than
     * trusting the varargs slot -- a signature promising more arguments than
     * were passed leaves a stale value there. safe_utf() returns "" for
     * anything that is not one of our strings, and "" only matches "". */
    if (!strcmp(id->name, "equals") || !strcmp(id->name, "contentEquals"))
      return strcmp(s, safe_utf(va_arg(va, void *))) == 0;
    if (!strcmp(id->name, "equalsIgnoreCase"))
      return strcasecmp(s, safe_utf(va_arg(va, void *))) == 0;
    if (!strcmp(id->name, "compareTo"))
      return (juint)(int32_t)strcmp(s, safe_utf(va_arg(va, void *)));
    if (!strcmp(id->name, "startsWith")) {
      const char *p = safe_utf(va_arg(va, void *));
      return strncmp(s, p, strlen(p)) == 0;
    }
    if (!strcmp(id->name, "endsWith")) {
      const char *p = safe_utf(va_arg(va, void *));
      size_t ls = strlen(s), lp = strlen(p);
      return lp <= ls && strcmp(s + ls - lp, p) == 0;
    }
    if (!strcmp(id->name, "contains"))
      return strstr(s, safe_utf(va_arg(va, void *))) != NULL;
    if (!strcmp(id->name, "indexOf")) {
      const char *h = strstr(s, safe_utf(va_arg(va, void *)));
      return (juint)(int32_t)(h ? (int32_t)(h - s) : -1);
    }

    /* Java's String.hashCode is a SPECIFIED algorithm, not an opaque value:
     * s[0]*31^(n-1) + s[1]*31^(n-2) + ... Returning 0 for every string (the
     * old behaviour) collapses any HashMap the game builds into a single
     * bucket, and then resolution falls back on equals() -- which until now
     * was also broken. Cheap to do correctly, so do it correctly. */
    if (!strcmp(id->name, "hashCode")) {
      int32_t h = 0;
      for (const char *c = s; *c; c++) h = h * 31 + (unsigned char)*c;
      return (juint)h;
    }
  }
  /* Boxed PlayerPrefs value (Integer/Long/Boolean) from getAll(): unbox by the
   * receiver so only our own boxes are affected. intValue/longValue/booleanValue
   * all land here (CallInt/Long/BooleanMethod -> dispatch_int). */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  /* MotionEvent/KeyEvent getters: the engine resolves these via
   * GetObjectClass(event) -> java/lang/Object, so id->cls is NOT the real
   * class. Route on the receiver tag (mirrors the FakeString case above), or
   * touch getters silently fall through to act_int and return 0. */
  if (input_owns_recv(recv)) return input_dispatch_int(recv, id, va);
  if (unity_owns_class(id->cls) || unity_owns_recv(recv)) return unity_dispatch_int(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_int(recv, id, va);
  if (is_t2b(id->cls)) return t2b_int(id, va);
  if (is_mov(id->cls)) return mov_int(id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  if (input_owns_recv(recv)) return input_dispatch_float(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  log_app_upcall(id);
  if (name_has(id->cls, "FMODAudioDevice")) {
    debugPrintf("[fmod] FMODAudioDevice.%s() CALLED\n", id->name);
    /* Path A (driving fmodProcess) is blocked: the mixer's source buffer is
     * allocated only by the AudioTrack output start sequence the Java run() loop
     * drives, which never runs here -- so fmodProcess always copies from a null
     * source (Data Abort at +0x28), confirmed even after a 120-frame warmup.
     * Pump left in the tree but disabled; audio is moving to FMOD OutputOpenSL
     * (Path B), which FMOD drives natively via opensles.c. */
    if (0 && !strcmp(id->name, "start")) fmod_audio_start();
  }
  if (unity_owns_class(id->cls) || unity_owns_recv(recv)) { unity_dispatch_void(recv, id, va); return; }
  if (is_mov(id->cls)) { mov_void(id, va); return; }
  act_void(id, va);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  /* Answering java/lang/Object for everything meant Unity's reflection
   * resolved File.getAbsolutePath as Object.getAbsolutePath, the class-gated
   * dispatch never reached the File handler, and the call was approximated.
   * Ask unity_jni first; it knows what its handles are. */
  const char *uc = unity_owns_recv(obj) ? unity_class_of(obj) : NULL;
  if (uc) return intern_class(uc);
  if (nx_tag_of(obj) == BITMAP_TAG) return intern_class("android/graphics/Bitmap");
  if (nx_tag_of(obj) == TAG_STRING) return intern_class("java/lang/String");
  if (nx_tag_of(obj) == TAG_ID)     return intern_class("java/lang/reflect/Member");
  /* jni_make_object() tags its pooled objects TAG_CLASS (so free_ref leaves
   * them alone) and FakeObject shares FakeClass's layout, so the label IS the
   * class name: an Activity object answers android/app/Activity. Returning
   * java/lang/Class here -- the previous revision -- made every method on the
   * activity resolve as Class.getPackageName etc. and fall to approximation. */
  if (nx_tag_of(obj) == TAG_CLASS) {
    const char *n = safe_class_name(obj);
    return intern_class((*n && strcmp(n, "obj")) ? n : "java/lang/Object");
  }
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

/* String(byte[][,charset]) constructor: Unity builds PlayerPrefs keys as
 * bytes -> new String(bytes, charset) -> Uri.encode(...). Returning a
 * content-less object made every encoded key empty, so all such prefs collided
 * under "" (corrupted Screenmanager resolution prefs -> bad resolution ->
 * crash). Decode the byte array (UTF-8) into a real FakeString. Other ctors are
 * unaffected (still a labelled object). */
extern void *unity_new_file(void *a0, void *a1);   /* unity_jni.c */
static void *new_object_dispatch2(void *cls, void *mid, void *a0, void *a1) {
  const char *cn = class_name_of(cls);
  /* new File(String) / new File(File, String) / new File(String, String).
   * Clone Hero does `new File(publicDocumentsDir, "Clone Hero")`, so the
   * two-argument forms matter. unity_new_file() reads either arg as a path
   * (a File handle or a String) and joins them. */
  if (cn && strstr(cn, "java/io/File")) {
    FakeID *m = mid;
    int two = m && strstr(m->sig, ";L") != NULL;      /* (Ljava/..;Ljava/..;)V */
    return unity_new_file(a0, two ? a1 : NULL);
  }
  return NULL;
}
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  if (cn && strstr(cn, "java/lang/String")) {
    FakeID *m = mid;
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  return jni_make_object(cn);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  va_list va; va_start(va, mid);
  void *a0 = va_arg(va, void *); void *a1 = va_arg(va, void *); va_end(va);
  void *r = new_object_dispatch2(cls, mid, a0, a1);
  return r ? r : new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env;
  void *a0 = va_arg(va, void *); void *a1 = va_arg(va, void *);
  void *r = new_object_dispatch2(cls, mid, a0, a1);
  return r ? r : new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* IsInstanceOf (slot 32). The unimpl stub returned 0 (false), trapping the game
 * in a per-frame retry loop on the black screen: it does obj=jniCall(); if
 * (IsInstanceOf(obj, Expected)) proceed; else retry. We can't track the runtime
 * type of opaque fake jobjects, so answer optimistically: per the JNI spec a
 * NULL object is an instance of any class, and for our fake objects assuming the
 * cast succeeds lets the game move forward instead of spinning. Logged (capped)
 * so we can see which class it is keying on. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  /* nativeInjectEvent classifies the event by instanceof KeyEvent / MotionEvent
   * and picks its handler accordingly. If we blindly return 1, the KeyEvent
   * check (which it does first) matches our touch event and it gets read as a
   * key (getKeyCode) and dropped. Answer by the handle's real kind. */
  if (input_owns_recv(obj)) {
    if (strstr(cn, "MotionEvent")) return input_recv_is_motion(obj) ? 1 : 0;
    if (strstr(cn, "KeyEvent"))    return input_recv_is_motion(obj) ? 0 : 1;
    /* InputEvent base class, or class names collapsed by pool overflow: both
     * kinds are InputEvents, so 1 is safe for the base; overflow is now logged. */
    return 1;
  }
  /* Boxed PlayerPrefs values from getAll(): Unity reads each value with
   * IsInstanceOf(value, Integer/Long/Float/Boolean/String) then unboxes. These
   * MUST be exact or every value is misread as the first type checked. */
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (nx_tag_of(obj) == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
    /* other classes: fall through to the optimistic answer below */
  }
  static int logn = 0;
  if (logn < 16) { logn++;
    debugPrintf("JNI: IsInstanceOf(obj=%p, clazz=%s) -> 1\n", obj, cn); }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share class-aware dispatch) --------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

static uint64_t dispatch_long(void *recv, const FakeID *id, va_list va) {
  if (name_has(id->name, "nanoTime")) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
  if (name_has(id->name, "currentTimeMillis")) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
  }
  if (!unity_is_boxed(recv) && name_has(id->name, "longValue")) return g_frame_ns;
  return (uint64_t)dispatch_int(recv, id, va);
}
CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, uint64_t, dispatch_long)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}
/* CallNonvirtual<Object>Method{,V,A}: (env, obj, clazz, methodID, args) -- like the
 * virtual object call but with an extra clazz arg our dispatch ignores. Slot 65
 * (V form) is called on the PlayAssetDelivery/package path and was UNIMPL -> null. */
static void *j_CallNonvirtualObjectMethodV(void *env, void *recv, void *clazz, FakeID *id, va_list va) {
  (void)env; (void)clazz; return dispatch_object(recv, id, va);
}
static void *j_CallNonvirtualObjectMethod(void *env, void *recv, void *clazz, FakeID *id, ...) {
  (void)env; (void)clazz; va_list va; va_start(va, id);
  void *r = dispatch_object(recv, id, va); va_end(va); return r;
}
static void *j_CallNonvirtualObjectMethodA(void *env, void *recv, void *clazz, FakeID *id, const void *a) {
  (void)a; return j_CallNonvirtualObjectMethod(env, recv, clazz, id);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- Call<type>MethodA / NewObjectA (jvalue[] args) -------------------------
// SWIG bindings and AndroidJavaObject.CallStatic<T>()/Call<T>() marshal their
// arguments into a jvalue[] array and invoke the "A" variants. A va_list cannot
// be reconstructed from jvalue[] portably, so we forward to the variadic form
// with no varargs: the dispatch keys off the resolved method name and the
// object/value getters ignore positional args (defaulting to a non-null handle
// of the right class), which is what these init paths need. Previously these
// slots fell through to the unimplemented stub and returned 0/null, hanging the
// first scene (e.g. UNIMPL slot 116 == CallStaticObjectMethodA).
/* Count the arguments a JNI signature declares, and whether any is F/D. */
static int sig_arg_count(const char *sig, int *has_fp) {
  int n = 0;
  if (has_fp) *has_fp = 0;
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return 0;
  for (p++; *p && *p != ')'; p++) {
    if (*p == '[') continue;                      /* array marker */
    if (*p == 'L') { while (*p && *p != ';') p++; n++; continue; }
    if (*p == 'F' || *p == 'D') { if (has_fp) *has_fp = 1; }
    n++;
  }
  return n;
}

/* ======================================================================
 * jvalue[] -> varargs forwarding.
 *
 * THIS WAS THE CRASH. Unity's AndroidJavaObject.Call<T>/CallStatic<T> marshal
 * their arguments into a jvalue[] and invoke the "A" entry points. These
 * wrappers used to DROP that array and call the variadic form with no varargs
 * at all. Every va_arg in every handler then read x3..x7 -- whatever the
 * caller had left there. For years of this loader's lineage that was survivable
 * because the handlers mostly ignored positional args. The moment one read
 * three of them (ReflectionHelper.getFieldID: Class, String, String), the
 * "Class" was 0x6374696f00000000 -- ASCII "oitc" -- and dereferencing it was a
 * data abort in safe_class_name. Killer Bean hit the identical fault from the
 * identical code ("round 16") and landed this fix ("round 132").
 *
 * Forward exactly as many real varargs as the signature declares. Sound for
 * object/int/long/boolean/byte/char/short: a jvalue is 8 bytes, varargs slots
 * are 8-byte aligned, and va_arg(va,int) reads the low half on little-endian,
 * which is where jvalue.i lives.
 *
 * NOT sound for float/double: a jvalue holds an unpromoted 4-byte float in an
 * integer slot, but variadic FP args travel in v0-v7 and va_arg(va,double)
 * reads from there. When the signature has one, forward ZERO args -- the
 * explicit jvalue reads in the wrappers below cover the FP cases that matter,
 * and zeros are recoverable where garbage is not. Zeros also for >6 args or a
 * NULL array, so va_arg can never again read an uninitialised register.
 * ==================================================================== */
#define JVA_N()                                                               \
  int jva_fp = 0;                                                             \
  int jva_n = a ? sig_arg_count(id->sig, &jva_fp) : 0;                        \
  const uint64_t *jv = (const uint64_t *)a;                                   \
  if (jva_fp || jva_n > 6) jva_n = 0;                                         \
  const uint64_t z = 0

#define JVA_DISPATCH(DO, FN, E, R, ID)                                        \
  switch (jva_n) {                                                            \
    case 1:  DO FN(E, R, ID, jv[0]); break;                                   \
    case 2:  DO FN(E, R, ID, jv[0], jv[1]); break;                            \
    case 3:  DO FN(E, R, ID, jv[0], jv[1], jv[2]); break;                     \
    case 4:  DO FN(E, R, ID, jv[0], jv[1], jv[2], jv[3]); break;              \
    case 5:  DO FN(E, R, ID, jv[0], jv[1], jv[2], jv[3], jv[4]); break;       \
    case 6:  DO FN(E, R, ID, jv[0], jv[1], jv[2], jv[3], jv[4], jv[5]); break;\
    default: DO FN(E, R, ID, z, z, z, z, z, z); break;                        \
  }

static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  // getProperty()'s String key lives in jvalue[0] and does NOT survive the
  // va_list-less forward below, so pull it directly. FMOD's OpenSL output reads
  // PROPERTY_OUTPUT_FRAMES_PER_BUFFER through this "A" path; without the key it
  // got "" -> framesPerBuffer 0 -> FMOD error 60.
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  if (a && name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return (void *)((void *const *)a)[0];        /* identity, jvalue path */

  /* Unity ReflectionHelper member resolution arrives HERE, not through the
   * varargs entry: AndroidJNISafe.CallStaticObjectMethod takes a
   * Span<jvalue>. Read the arguments from the array while we still have it,
   * with each one validated before use. Arities differ:
   *   getFieldID / getMethodID (Class, String name, String sig, boolean)
   *   getConstructorID         (Class, String sig)
   * dispatch_object has a mirror of this for a genuine varargs caller; with
   * JVA_DISPATCH forwarding real args it would work too, but reading straight
   * from the array involves no varargs at all and is the safer path. */
  if (a && name_has(id->cls, "unity3d/player/ReflectionHelper") &&
      (name_has(id->name, "getFieldID") || name_has(id->name, "getMethodID") ||
       name_has(id->name, "getConstructorID"))) {
    void *const *jv = (void *const *)a;
    int is_ctor = name_has(id->name, "getConstructorID");
    const char *cn = safe_class_name(jv[0]);
    const char *s1 = safe_utf(jv[1]);
    const char *s2 = is_ctor ? "" : safe_utf(jv[2]);
    const char *mn = is_ctor ? "<init>" : s1;
    const char *ms = is_ctor ? s1 : s2;
    static int nlog = 0;
    if (nlog < 16) { nlog++;
      debugPrintf("[jni] %s [A] -> %s.%s%s\n", id->name, *cn ? cn : "?", *mn ? mn : "?", ms); }
    return get_id(*cn ? cn : "java/lang/Object", *mn ? mn : "?", ms);
  }

  { JVA_N(); JVA_DISPATCH(return, j_CallObjectMethod, e, r, id); }
}
static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){
  JVA_N(); JVA_DISPATCH(return, j_CallBooleanMethod, e, r, id);
}
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  // parseInt/parseLong via the jvalue[] path: read the String from jvalue[0].
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u [A]\n", id->name, s ? s : "", v);
    return v;
  }
  { JVA_N(); JVA_DISPATCH(return, j_CallIntMethod, e, r, id); }
}
static uint64_t j_CallLongMethodA(void *e, void *r, FakeID *id, const void *a){
  JVA_N(); JVA_DISPATCH(return, j_CallLongMethod, e, r, id);
}
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){
  JVA_N(); JVA_DISPATCH(return, j_CallFloatMethod, e, r, id);
}
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){
  JVA_N(); JVA_DISPATCH((void), j_CallVoidMethod, e, r, id);
}
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  /* jvalue[] has exactly as many slots as the signature has arguments, so
   * only read a[1] when the sig actually declares a second one. */
  FakeID *m = mid; int two = m && strstr(m->sig, ";L") != NULL;
  void *a0 = a ? ((void *const *)a)[0] : NULL;
  void *a1 = (a && two) ? ((void *const *)a)[1] : NULL;
  void *r = new_object_dispatch2(cls, mid, a0, a1);
  return r ? r : new_object_dispatch(cls, mid, a0); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) { // naive UTF-16 -> UTF-8 (BMP)
    const uint32_t c = u[i];
    if (c < 0x80) tmp[o++] = (char)c;
    else if (c < 0x800) { tmp[o++] = 0xC0 | (c >> 6); tmp[o++] = 0x80 | (c & 0x3F); }
    else { tmp[o++] = 0xE0 | (c >> 12); tmp[o++] = 0x80 | ((c >> 6) & 0x3F); tmp[o++] = 0x80 | (c & 0x3F); }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// GetStringUTFRegion: the engine reads ALL its strings through this (not
// GetStringUTFChars), so it must work. Copies the [start, start+len) region as
// modified UTF-8 into buf. Our strings are ASCII (paths / archive names), where
// UTF-16 char offsets == UTF-8 byte offsets, so a byte copy is exact.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
// GetStringRegion: UTF-16 variant; widen ASCII bytes into jchar (uint16) buf.
/* UTF-8 -> UTF-16 (code units, with surrogate pairs above the BMP). Named
 * jni_ because libnx already exports a utf8_to_utf16() with a different
 * signature in switch/runtime/util/utf.h. Decodes
 * `s` into `out` (capacity `cap` code units, may be NULL to count only) and
 * returns the code-unit count. Malformed bytes decode as U+FFFD, one per byte,
 * so the count always agrees with utf16_len() and the buffer never overruns. */
static int jni_utf8_to_utf16(const char *s, uint16_t *out, int cap) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    uint32_t c; int extra;
    if      (*p < 0x80)           { c = *p;          extra = 0; }
    else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F;   extra = 1; }
    else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F;   extra = 2; }
    else if ((*p & 0xF8) == 0xF0) { c = *p & 0x07;   extra = 3; }
    else                          { c = 0xFFFD;       extra = 0; }
    p++;
    for (int i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) { c = 0xFFFD; break; }
      c = (c << 6) | (*p++ & 0x3F);
    }
    /* n always advances; the store is conditional on having a buffer with
     * room, so the same call both counts (out == NULL) and fills. */
    if (c >= 0x10000) {
      c -= 0x10000;
      if (out && n < cap) { out[n] = (uint16_t)(0xD800 | (c >> 10)); }
      n++;
      if (out && n < cap) { out[n] = (uint16_t)(0xDC00 | (c & 0x3FF)); }
      n++;
    } else {
      if (out && n < cap) { out[n] = (uint16_t)c; }
      n++;
    }
  }
  return n;
}

/* ======================================================================
 * GetStringChars / ReleaseStringChars (slots 165/166) and the Critical pair.
 *
 * THESE WERE MISSING. This is how C# reads a Java string: .NET strings are
 * UTF-16, so Unity's AndroidJNI converts a jstring with GetStringChars, not
 * GetStringUTFChars. With the slot unimplemented every string this loader
 * handed back to the game's own code -- a field signature, a path, a
 * property -- was unreadable on the C# side. The log showed the pair firing
 * as "UNIMPL slot 165 / 166" right after every ReflectionHelper call: Unity
 * was trying to read getFieldSignature()'s result. It got nothing, so the next
 * lookup went out with an EMPTY signature even though we had returned the
 * right one. That empty sig is the "" in `DIRECTORY_DOCUMENTS ` in the log.
 *
 * Returned buffers are heap-allocated UTF-16, NUL-terminated for callers that
 * ignore the length, and freed by the matching Release. isCopy = true.
 * ==================================================================== */
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *isCopy) {
  (void)env;
  const char *s = obj_str(jstr);
  int n = jni_utf8_to_utf16(s, NULL, 0);
  uint16_t *buf = malloc(((size_t)n + 1) * sizeof *buf);
  if (!buf) { if (isCopy) *isCopy = 0; return NULL; }
  jni_utf8_to_utf16(s, buf, n);
  buf[n] = 0;
  if (isCopy) *isCopy = 1;
  return buf;
}
static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  free((void *)chars);
}
static const uint16_t *j_GetStringCritical(void *env, void *jstr, uint8_t *isCopy) {
  return j_GetStringChars(env, jstr, isCopy);
}
static void j_ReleaseStringCritical(void *env, void *jstr, const uint16_t *chars) {
  j_ReleaseStringChars(env, jstr, chars);
}

static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf || len <= 0) return;
  /* Region is in UTF-16 code units, so decode fully and slice -- the old
   * byte-for-byte copy was only right for ASCII. */
  const char *s = obj_str(jstr);
  int total = jni_utf8_to_utf16(s, NULL, 0);
  if (start < 0) start = 0;
  if (start > total) start = total;
  if (start + len > total) len = total - start;
  if (len <= 0) return;
  uint16_t *tmp = malloc((size_t)total * sizeof *tmp);
  if (!tmp) return;
  jni_utf8_to_utf16(s, tmp, total);
  memcpy(buf, tmp + start, (size_t)len * sizeof *buf);
  free(tmp);
}
// GetStringLength must return the UTF-16 code-unit count, not the byte count
// (CJK text is multi-byte in UTF-8); engine code sizes UTF-16 buffers with it.
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  uint32_t t = nx_tag_of(a);
  if (t == TAG_PRIARR || t == TAG_OBJARR)
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
void *jni_classloader_obj(void) { return get_classloader_obj(); }
void *jni_make_object_array(int len) { return j_NewObjectArray(fake_env, len, NULL, NULL); }
void *jni_make_int_array(const int *src, int len) {
  int *d = calloc(len ? len : 1, sizeof *d);
  if (src && len > 0) memcpy(d, src, (size_t)len * sizeof *d);
  return make_pri_array_adopt(d, len, 4);
}
void *jni_make_bool_array_filled(int len, int value) {
  uint8_t *d = calloc(len ? len : 1, 1);
  if (value) memset(d, 1, (size_t)len);
  return make_pri_array_adopt(d, len, 1);
}
int jni_array_length(void *arr) {
  FakePriArray *a = arr;
  uint32_t t = nx_tag_of(a);
  return (t == TAG_PRIARR || t == TAG_OBJARR) ? a->len : 0;
}
void  jni_object_array_set(void *arr, int i, void *v) {
  FakeObjArray *a = arr;
  if (nx_tag_of(a) == TAG_OBJARR && i >= 0 && i < a->len && ptr_plausible(a->items)) a->items[i] = v;
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  if (nx_tag_of(a) != TAG_OBJARR || i < 0 || i >= a->len) return NULL;
  return ptr_plausible(a->items) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (nx_tag_of(a) == TAG_OBJARR && i >= 0 && i < a->len && ptr_plausible(a->items))
    a->items[i] = val;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (nx_tag_of(a) == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (nx_tag_of(a) == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (nx_tag_of(a) == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- fields -----------------------------------------------------------------
// The engine and the game DO read Java fields: android.os.Build.* (device id),
// Build.VERSION.SDK_INT (API gating), PackageInfo.versionName/versionCode (the
// Application.version the boot path logs as blank today), DisplayMetrics.*, and
// Configuration.*. Returning null/0 universally (the old stub) blanks the app
// version -- which can throw in version-parsing boot code -- and zeroes display
// metrics. Route every field read through a name-based dispatcher. fid is the
// FakeID GetFieldID handed back, so cls/name/sig are all available.
//
// Placeholders marked CHECK are safe defaults, not the shipped values. NOTE:
// that was true for Zookeeper; Clone Hero DOES gate boot on versionName -- see
// app_version_name() below.
/* PackageInfo.versionName == Application.version, and Clone Hero GATES ON IT.
 *
 * GlobalVariables.Awake (dump.cs, disassembled in boot 21):
 *     if (PlayerPrefs.GetString("saVersion") == Application.version) {
 *         if (instance != null) Destroy(this) else { instance = this; DontDestroyOnLoad }
 *     } else SceneManager.LoadScene(unpacking);      // instance never assigned
 *
 * "saVersion" is written from streamingAssets.yml's GameVersion --
 * "v1.1.0.6142-final" -- which on a real device equals the APK's versionName.
 * This define was "2.1.6" (the game's INTERNAL version, from its session log
 * header), so the comparison failed on every boot: the unpack scene reloaded
 * each time, GlobalVariables.instance stayed null, and every menu script's
 * Awake/OnEnable/Start threw NullReferenceException. Two symptoms that had
 * looked unrelated -- the eternal re-unpack and the black screen -- were this
 * one string.
 *
 * Read it from the manifest at first use so it is always the value the game
 * compares against; the literal is only a fallback. */
#define APP_VERSION_NAME_FALLBACK "v1.1.0.6142-final"
static const char *app_version_name(void) {
  static char ver[96]; static int loaded = 0;
  if (loaded) return ver;
  loaded = 1;
  snprintf(ver, sizeof ver, "%s", APP_VERSION_NAME_FALLBACK);
  FILE *f = fopen(nx_path("/assets/streamingAssets.yml"), "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
      const char *p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (!strncmp(p, "GameVersion:", 12)) {
        p += 12; while (*p == ' ') p++;
        size_t L = strcspn(p, "\r\n");
        if (L && L < sizeof ver) { memcpy(ver, p, L); ver[L] = 0; }
        break;
      }
    }
    fclose(f);
  }
  debugPrintf("[jni] Application.version (PackageInfo.versionName) = \"%s\"%s\n", ver,
              strcmp(ver, APP_VERSION_NAME_FALLBACK) ? " (from streamingAssets.yml)" : "");
  return ver;
}
#define APP_VERSION_NAME app_version_name()
#define APP_VERSION_CODE 45        /* real value from split_config arm64 manifest */
#define NX_SDK_INT       33        /* Android 13 -- high enough to pass any minSdk gate  */

static int fld_is(const FakeID *id, const char *cls_sub, const char *name) {
  return name_has(id->cls, cls_sub) && !strcmp(id->name, name);
}

// One-line-per-unique-field diagnostic: tells the next run exactly which Java
// fields the game reads (and lets us confirm versionName/currentActivity/etc.
// are being exercised). Dedup by interned name pointer, like log_app_upcall.
static void log_field_read(const FakeID *id, char kind) {
  static const void *seen[128]; static int seen_n = 0;
  for (int i = 0; i < seen_n; i++) if (seen[i] == id->name) return;
  if (seen_n < 128) seen[seen_n++] = id->name;
  debugPrintf("[jni] field(%c): %s.%s %s\n", kind, id->cls, id->name, id->sig);
}

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  /* software-keyboard result: the engine reads it as a String field */
  if (n && (!strcmp(n, "text") || !strcmp(n, "mText") ||
            !strcmp(n, "inputText") || !strcmp(n, "m_Text") ||
            name_has(n, "KeyboardText") || name_has(n, "EditBoxText"))) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text FIELD %s.%s -> \"%s\"\n", c ? c : "?", n, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  // PackageInfo / ApplicationInfo version string
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  // UnityPlayer statics: currentActivity is THE Activity -- null here NPEs every
  // UnityPlayer.currentActivity.getXxx() in managed code, so hand back a live
  // (opaque) Activity that our method dispatch then services.
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
  }
  // AudioManager.PROPERTY_OUTPUT_* are static String field keys the engine reads
  // just before AudioManager.getProperty(key) to size FMOD's audio path. Return
  // the real Android property-name strings AND record which one was read
  // (g_last_output_prop) so getProperty() can answer even when the key argument
  // is lost on the JNI call path (see getproperty_value). These are AudioManager
  // fields, NOT UnityPlayer -- gating them on the wrong class meant they never
  // matched, getProperty saw "", and FMOD got framesPerBuffer 0 -> error 60.
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  // Context.*_SERVICE name constants -> the strings getSystemService() expects
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  // Environment.MEDIA_MOUNTED MUST equal getExternalStorageState()'s return
  // ("mounted", set in act_object) or the storage check fails and save data is
  // disabled. Keep both in lockstep.
  if (name_has(c, "os/Environment")) {
    /* Environment.DIRECTORY_* -- the public directory names. Clone Hero keeps
     * its library under DIRECTORY_DOCUMENTS ("Documents/Clone Hero" on a
     * phone). getExternalStoragePublicDirectory(name) in unity_jni.c resolves
     * these to <data root>/<name>, so "Documents" here becomes
     * sdmc:/switch/<folder>/Documents there. */
    if (!strcmp(n, "DIRECTORY_DOCUMENTS"))     return jni_make_string("Documents");
    if (!strcmp(n, "DIRECTORY_DOWNLOADS"))     return jni_make_string("Download");
    if (!strcmp(n, "DIRECTORY_MUSIC"))         return jni_make_string("Music");
    if (!strcmp(n, "DIRECTORY_PICTURES"))      return jni_make_string("Pictures");
    if (!strcmp(n, "DIRECTORY_MOVIES"))        return jni_make_string("Movies");
    if (!strcmp(n, "DIRECTORY_DCIM"))          return jni_make_string("DCIM");
    if (!strcmp(n, "DIRECTORY_PODCASTS"))      return jni_make_string("Podcasts");
    if (!strcmp(n, "DIRECTORY_RINGTONES"))     return jni_make_string("Ringtones");
    if (!strcmp(n, "DIRECTORY_ALARMS"))        return jni_make_string("Alarms");
    if (!strcmp(n, "DIRECTORY_NOTIFICATIONS")) return jni_make_string("Notifications");
    if (!strcmp(n, "DIRECTORY_AUDIOBOOKS"))    return jni_make_string("Audiobooks");
    if (!strcmp(n, "DIRECTORY_SCREENSHOTS"))   return jni_make_string("Screenshots");
    if (!strcmp(n, "DIRECTORY_RECORDINGS"))    return jni_make_string("Recordings");
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  // android.os.Build identity strings (all public static final String)
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  // Any other String-typed field -> "" (non-null avoids NPEs in string ops).
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  // No signature at all (Unity's reflection fallback asked by name only): a
  // SCREAMING_CASE constant is a String far more often than not, and "" is
  // survivable where NULL was the last link in a NullReferenceException.
  if (!id->sig[0]) {
    int caps = 1;
    for (const char *c = id->name; *c; c++)
      if (!((*c >= 'A' && *c <= 'Z') || *c == '_' || (*c >= '0' && *c <= '9'))) { caps = 0; break; }
    if (caps) return jni_make_string("");
  }
  // Any other object field stays null; array fields handled by the caller.
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "what") && name_has(c, "Message")) return (juint)g_msg_what;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  // UnityPlayer integer statics
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return 320;
    if (!strcmp(n, "widthPixels")) return screen_width;   /* real panel (landscape) */
    if (!strcmp(n, "heightPixels"))return screen_height;
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  if (name_has(c, "Configuration") && !strcmp(n, "orientation")) return 2;  /* LANDSCAPE */
  // DisplayMetrics integer fields (width/height/dpi)
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return screen_width;   /* real panel (landscape) */
    if (!strcmp(n, "heightPixels")) return screen_height;
    if (!strcmp(n, "densityDpi"))   return 320;    /* xhdpi bucket                */
  }
  return 0;
}

/* DisplayMetrics.density / xdpi / ydpi / scaledDensity are float fields. 0 would
 * make dp->px scaling collapse, so hand back a sane xhdpi density (2.0). */
static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  (void)fld_is;
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  log_field_read((const FakeID *)fid, 'O');
  { const FakeID *f = (const FakeID *)fid;   /* name the field the game really reads */
    if (editbox_text() && editbox_text()[0]) {
      static unsigned seen;
      if (seen < 16) { seen++;
        debugPrintf("[kbd] objfield read: %s.%s sig=%s\n",
                    f->cls, f->name,
                    f->sig); } } }
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  log_field_read((const FakeID *)fid, 'I');
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }

// --- reflection bridge (proxy support) --------------------------------------
// Unity's AndroidJavaProxy / JNIBridge.newInterfaceProxy converts the reflected
// Method/Field objects of an interface into jmethod/jfieldIDs via these. Slot 7
// (FromReflectedMethod) and slot 8 (FromReflectedField) were unimplemented, so
// the proxy couldn't bind its methods (the "UNIMPL slot 7" lines). We don't
// carry real reflection, but returning a non-null opaque ID lets the proxy set
// up and be stored; if such a proxy callback is ever actually invoked it routes
// through act_* and no-ops, which is the right behaviour for our stubbed events.
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env;
  if (nx_tag_of(m) == TAG_ID) return m;   /* proxy_run passes the real run() id */
  return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env;
  /* ReflectionHelper.getFieldID (dispatch_object) hands back our own FakeID as
   * the "reflected Field". Pass it through, exactly as FromReflectedMethod does
   * for methods. The old body returned a fixed generic ID for EVERY field,
   * throwing the field name away -- so a later GetStaticObjectField could not
   * know it was being asked for DIRECTORY_DOCUMENTS. */
  if (nx_tag_of(f) == TAG_ID) return f;
  return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

// --- misc -------------------------------------------------------------------

/* JNI registers org/fmod/FMODAudioDevice's native bridge (fmodGetInfo /
 * fmodProcess / fmodProcessMicData) here -- these are file-local in libunity, so
 * RegisterNatives is the only place their addresses are exposed. Capture them so
 * a native playback thread can pull PCM from FMOD (the Java run() loop never runs
 * because we have no JVM). */
typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod_;
void *g_fmod_getinfo = 0, *g_fmod_process = 0, *g_fmod_micdata = 0;
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  const char *cn = class_name_of(cls);
  const JNINativeMethod_ *m = methods;
  int is_fmod = name_has(cn, "fmod") || name_has(cn, "FMOD");
  debugPrintf("[jni] RegisterNatives %s (%d methods)%s\n", cn, n, is_fmod ? "  <-- fmod" : "");
  if (m && name_has(cn, "unity3d/player/UnityPlayer")) {
    for (int i = 0; i < n; i++) {
      if (!m[i].name) continue;
      if (!strcmp(m[i].name, "nativeSetInputString"))    g_u_setInputString  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetInputSelection")) g_u_setInputSel = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputClosed"))   g_u_softClosed  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputCanceled")) g_u_softCancel  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetKeyboardIsVisible")) g_u_kbdVisible = m[i].fn;
    }
    debugPrintf("[kbd] captured Unity soft-input natives: str=%p sel=%p closed=%p cancel=%p vis=%p\n",
                g_u_setInputString, g_u_setInputSel, g_u_softClosed,
                g_u_softCancel, g_u_kbdVisible);
  }
  if (m) {   /* dump the whole table once per class: the keyboard callback is in here */
    static unsigned dumped;
    for (int i = 0; i < n && dumped < 200; i++, dumped++)
      debugPrintf("[natives] %s.%s %s -> %p\n", cn,
                  m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", m[i].fn);
  }
  if ((name_has(cn, "jnibridge") || name_has(cn, "JNIBridge")) && m) {
    for (int i = 0; i < n; i++) if (m[i].name && name_has(m[i].name, "invoke")) g_jnibridge_invoke = (jnibridge_invoke_fn)m[i].fn;
    debugPrintf("[jni] captured JNIBridge invoke=%p\n", (void *)g_jnibridge_invoke);
  }
  if (is_fmod && m) {
    for (int i = 0; i < n; i++) {
      debugPrintf("[jni]   %s %s -> %p\n",
                  m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", m[i].fn);
      if (!m[i].name) continue;
      if      (!strcmp(m[i].name, "fmodGetInfo"))        g_fmod_getinfo = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcess"))        g_fmod_process = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcessMicData")) g_fmod_micdata = m[i].fn;
    }
    debugPrintf("[fmod] captured getInfo=%p process=%p micData=%p\n",
                g_fmod_getinfo, g_fmod_process, g_fmod_micdata);
  }
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; jni_approx_checked(); return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

// ---------------------------------------------------------------------------
// FMOD native-audio pump
// ---------------------------------------------------------------------------
// fmodProcess(env, this, ByteBuffer) renders one fixed-size FMOD mixer block
// (size comes from the output singleton set up at start(), NOT from the buffer
// capacity) straight into env->GetDirectBufferAddress(ByteBuffer), then returns
// 0. We have no JVM, so the Java FMODAudioDevice.run() loop never calls it --
// this native thread does instead, and pushes the PCM to the SDL sink.
//
// The only JNIEnv entry fmodProcess uses is GetDirectBufferAddress (slot 230);
// fmodGetInfo(which) uses none. So the shim only has to hand back our staging
// buffer and feed the captured function pointers a non-NULL `this`/buffer token.

#define FMOD_STAGING_BYTES (64 * 1024)   // generous: must exceed one mixer block
static unsigned char g_fmod_staging[FMOD_STAGING_BYTES];
static int  g_fmod_bb_token   = 0;       // stand-in jobject for the ByteBuffer
static int  g_fmod_this_token = 0;       // stand-in jobject for `this`
static int  g_fmod_started    = 0;

typedef int (*fmod_getinfo_fn)(void *env, void *thiz, int which);
typedef int (*fmod_process_fn)(void *env, void *thiz, void *bytebuffer);

/* slot 230. SCOPED TO THE FMOD BUFFER, not "every ByteBuffer".
 *
 * These used to ignore `buf` entirely and hand back g_fmod_staging for any
 * caller. That is safe only while FMOD is the sole user, and it is not: the
 * approximation ledger shows java/nio/IntBuffer.allocate() being called and
 * INSPECTED, so something else is asking this port for buffers.
 *
 * Handing an unrelated caller the AUDIO STAGING BUFFER is a memory-corruption
 * primitive, and a nasty one to diagnose: g_fmod_staging is a static array, so
 * the address is at a FIXED OFFSET and would corrupt the same bytes on every
 * run -- which is exactly the deterministic signature the recurring crash has
 * (AUDIT sec 25). Worse, the capacity call claimed FMOD_STAGING_BYTES for that
 * unrelated buffer too, so a caller that allocated a small IntBuffer would
 * believe it had the whole staging area to write into.
 *
 * NULL is the CORRECT JNI answer for anything that is not a direct buffer --
 * the spec says so and callers must null-check it -- and it is a far better
 * failure than silently sharing the audio buffer. */
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env;
  if (buf == (void *)&g_fmod_bb_token) return g_fmod_staging;
  { static int warned = 0;
    if (!warned) { warned = 1;
      debugPrintf("[jni] GetDirectBufferAddress on a NON-FMOD buffer %p -> NULL "
                  "(was: the audio staging buffer -- see AUDIT sec 27)\n", buf); } }
  return NULL;
}
/* slot 231 (defensive -- the disasm shows fmodProcess never calls it). Must
 * agree with the address call: 0 capacity for the buffers that get NULL. */
static long j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env;
  return (buf == (void *)&g_fmod_bb_token) ? (long)FMOD_STAGING_BYTES : -1;
}

// Discover how many bytes fmodProcess actually wrote, once, by sentinel-fill.
// Silence (0x0000) still differs from the 0xCD fill, so a silent first block is
// detected correctly.
static int probe_block_bytes(fmod_process_fn process, int frame_bytes) {
  memset(g_fmod_staging, 0xCD, FMOD_STAGING_BYTES);
  process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
  int last = -1;
  for (int i = FMOD_STAGING_BYTES - 1; i >= 0; i--) {
    if (g_fmod_staging[i] != 0xCD) { last = i; break; }
  }
  if (last < 0) return 0;
  int bytes = last + 1;
  if (frame_bytes > 0)                    // round up to a whole frame
    bytes = ((bytes + frame_bytes - 1) / frame_bytes) * frame_bytes;
  if (bytes > FMOD_STAGING_BYTES) bytes = FMOD_STAGING_BYTES;
  return bytes;
}

static int16_t block_peak(int bytes) {
  const int16_t *s = (const int16_t *)g_fmod_staging;
  int n = bytes / 2; int16_t peak = 0;
  for (int i = 0; i < n; i++) {
    int16_t v = s[i] < 0 ? (int16_t)-s[i] : s[i];
    if (v > peak) peak = v;
  }
  return peak;
}

static void *fmod_audio_thread(void *arg) {
  (void)arg;
  fmod_getinfo_fn getinfo = (fmod_getinfo_fn)g_fmod_getinfo;
  fmod_process_fn process = (fmod_process_fn)g_fmod_process;
  if (!process) { debugPrintf("[fmod] pump: no process ptr, abort\n"); return NULL; }

  int rate = 48000, channels = 2;
  if (getinfo) {
    int r = getinfo(fake_env, &g_fmod_this_token, 0);
    int c = getinfo(fake_env, &g_fmod_this_token, 1);
    debugPrintf("[fmod] getInfo: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d\n",
                r, c, getinfo(fake_env, &g_fmod_this_token, 2),
                getinfo(fake_env, &g_fmod_this_token, 3),
                getinfo(fake_env, &g_fmod_this_token, 4));
    if (r >= 8000 && r <= 192000) rate = r;
    if (c == 1 || c == 2 || c == 6) channels = c;
  }
  const int frame_bytes = channels * 2; // S16

  // CRITICAL: start() fires before Unity's render loop has driven a single
  // System::update(), so the FMOD mixer's DSP buffers aren't allocated yet --
  // calling fmodProcess now faults (null deref deep in the mix/copy path). Wait
  // for the engine to tick a batch of frames (each drives a System::update that
  // finalizes the mixer) before the first call. A faulting call can't be caught
  // (no working SEH here), so this warmup is the only protection.
  extern uint32_t port_frame_count(void);
  #define FMOD_WARMUP_FRAMES 120u
  uint32_t f0 = port_frame_count();
  debugPrintf("[fmod] warmup: waiting %u frames (start frame=%u)\n", FMOD_WARMUP_FRAMES, f0);
  for (int guard = 0; guard < 1500; guard++) {            // ~15s hard cap
    if (port_frame_count() - f0 >= FMOD_WARMUP_FRAMES) break;
    svcSleepThread(10000000ULL);                          // 10 ms
  }
  debugPrintf("[fmod] warmup done at frame=%u, probing\n", port_frame_count());

  // start() may still be wiring the FMOD output singleton; fmodProcess writes
  // nothing until it's live. Retry the probe briefly before giving up.
  int block = 0;
  for (int tries = 0; tries < 100 && block <= 0; tries++) {
    block = probe_block_bytes(process, frame_bytes);
    if (block <= 0) svcSleepThread(10000000ULL); // 10 ms
  }
  debugPrintf("[fmod] pump start: %d Hz, %d ch, block=%d bytes (%d frames)\n",
              rate, channels, block, block / (frame_bytes ? frame_bytes : 1));
  if (block <= 0) {
    debugPrintf("[fmod] pump: fmodProcess wrote nothing after retries, abort\n");
    return NULL;
  }

  int dev_rate = audio_fmod_open(rate, channels);
  if (!dev_rate) { debugPrintf("[fmod] pump: device open failed, abort\n"); return NULL; }

  // pace to realtime via the device queue; target ~4 blocks buffered.
  const uint32_t hi = (uint32_t)block * 6;
  const uint32_t lo = (uint32_t)block * 3;
  long iters = 0;
  for (;;) {
    while (audio_fmod_queued() > hi)
      svcSleepThread(2000000ULL); // 2 ms
    // refill toward the low watermark
    do {
      process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
      uint32_t q = audio_fmod_write(g_fmod_staging, block);
      if (iters < 4) {
        debugPrintf("[fmod] block %ld: peak=%d queued=%u\n",
                    iters, (int)block_peak(block), q);
      }
      iters++;
      if (q > hi) break;
    } while (audio_fmod_queued() < lo);
    svcSleepThread(2000000ULL); // 2 ms
  }
  return NULL;
}

// Called from dispatch_void when FMODAudioDevice.start() fires (pointers are
// already captured by then -- RegisterNatives precedes start()).
void fmod_audio_start(void) {
  if (g_fmod_started) return;
  if (!g_fmod_process) { debugPrintf("[fmod] start(): process ptr not captured yet\n"); return; }
  g_fmod_started = 1;
  pthread_t th;
  if (pthread_create(&th, NULL, fmod_audio_thread, NULL) != 0) {
    debugPrintf("[fmod] pthread_create failed\n");
    g_fmod_started = 0;
    return;
  }
  pthread_detach(th);
  debugPrintf("[fmod] native playback thread launched\n");
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
/* ZOOKEEPER DX port: accessors so unity_jni.c/unity_input.c can read into the
 * (otherwise static) FakeString / FakePriArray without duplicating the structs. */
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (nx_tag_of(a) == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return safe_utf(s);
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  jni_fill_unimpl(env_table); // indexed stubs: log the exact unimplemented slot

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // "A" (jvalue[]) variants -- instance
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[64]  = (void *)j_CallNonvirtualObjectMethod;    // was UNIMPL
  env_table[65]  = (void *)j_CallNonvirtualObjectMethodV;   // was UNIMPL slot 65 (PAD path)
  env_table[66]  = (void *)j_CallNonvirtualObjectMethodA;   // was UNIMPL
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // "A" (jvalue[]) variants -- static (SWIG / AndroidJavaObject.CallStatic<T>)
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;       /* was UNIMPL: C# could not read our strings */
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[224] = (void *)j_GetStringCritical;
  env_table[225] = (void *)j_ReleaseStringCritical;
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[230] = (void *)j_GetDirectBufferAddress;  // fmodProcess drains via this
  env_table[231] = (void *)j_GetDirectBufferCapacity; // defensive (unused by fmodProcess)

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
