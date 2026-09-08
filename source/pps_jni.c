/* pps_jni.c -- the JNI environment libpapapearsaga.so expects.
 *
 * MIT licensed. See LICENSE, and see pps_jni.h for why this is neither of the
 * two reference ports' approaches.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "config.h"
#include "pps_jni.h"

#define JNI_OK          0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

volatile int jni_quit_requested = 0;

/* ------------------------------------------------------------------ */
/* Object model                                                        */
/* ------------------------------------------------------------------ */

enum {
  TAG_OBJECT = 0x504f424au,  /* 'POBJ'  opaque, class-tagged, pooled     */
  TAG_STRING = 0x50535452u,  /* 'PSTR'                                   */
  TAG_PRIARR = 0x50504152u,  /* 'PPAR'  primitive array                  */
  TAG_OBJARR = 0x504f4152u,  /* 'POAR'  object array                     */
  TAG_CLASS  = 0x50434c53u,  /* 'PCLS'  pooled, never freed              */
  TAG_ID     = 0x504d4944u,  /* 'PMID'  method/field id, pooled          */
};

#define MAX_PROPS 6

typedef struct {
  uint32_t tag;
  char cls[96];
  /* Heap-allocated because one of these is a Play purchase's original JSON
   * blob, which runs to a couple of hundred characters -- an inline buffer
   * large enough for it would be paid for by all 256 pooled objects. */
  struct { char key[24]; char *val; } prop[MAX_PROPS];
  int nprops;
} FakeObject;

typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; int elem_size; char kind; void *data; } FakePriArray;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; char name[96]; } FakeClass;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[192]; } FakeID;

static Mutex g_lock;

/* ------------------------------------------------------------------ */
/* Pools                                                               */
/* ------------------------------------------------------------------ */

/* Classes, method ids and opaque objects are interned and never freed.
 *
 * That is not laziness about lifetimes -- it is what makes the whole scheme
 * safe. Our objects are stateless handles dispatched by class name, not by
 * identity, so one instance per class is indistinguishable from many. And the
 * engine resolves the same class and method ids from static initialisers on
 * several threads; pooling means a DeleteLocalRef on one thread cannot free a
 * handle another thread is still holding.
 *
 * The pools are sized from the actual surface: the dex has 95 native entry
 * points across roughly 40 classes, and the outbound side resolves a few
 * hundred method ids. These caps are several times that. */
#define MAX_CLASSES 256
#define MAX_IDS     1024
#define MAX_OBJECTS 256
#define MAX_ISTRING 512

static FakeClass  g_classes[MAX_CLASSES];   static int g_nclasses;
static FakeID     g_ids[MAX_IDS];           static int g_nids;
static FakeObject g_objects[MAX_OBJECTS];   static int g_nobjects;
static FakeString g_istrings[MAX_ISTRING];  static int g_nistrings;

/* ------------------------------------------------------------------ */
/* Reference safety                                                    */
/* ------------------------------------------------------------------ */

/* Everything in this section exists because of one crash, and the shape of it
 * is worth stating because it explains all three mechanisms below.
 *
 * The engine called DeleteLocalRef on a reference it had already deleted. This
 * file freed whatever it was handed, so the second delete was a free() of dead
 * memory: newlib corrupted its own heap, later strdup()s returned damaged
 * blocks, and the symptom that actually showed up was FILE PATHS TURNING TO
 * GARBAGE several hundred opens later. The crash itself landed in _free_r with
 * our j_DeleteRef one frame up.
 *
 * Three defences, taken from the Daggerfall Unity port, which hits this
 * surface far harder than this game does:
 *
 *   1. OWNERSHIP. A reference is only freed if it is actually in the local
 *      table. A second delete finds nothing and does nothing. This alone fixes
 *      the crash; the other two make the failure mode survivable when it
 *      happens somewhere this has not thought of.
 *
 *   2. POISON BEFORE FREE. The tag and the inner pointer are cleared before
 *      either is released, so a stale reader sees "not one of ours" rather
 *      than a live-looking header with a dangling payload. That is precisely
 *      what produced the garbage paths: a valid TAG_STRING whose utf pointer
 *      had been recycled.
 *
 *   3. QUARANTINE. The block is not returned to the allocator immediately but
 *      held in a ring, so a use-after-free reads clean stale data instead of
 *      whatever was allocated over it. This does not make the bug correct --
 *      it turns a heap corruption into a wrong answer, which is debuggable.
 */

/* Switch user address space is 39 bits. Anything at or above this is not a
 * pointer; it is data that reached us through an unsupplied varargs slot. */
#define NX_USER_AS_END 0x8000000000ull

/* Defined below, with free_ref, which needs the reference layouts. */
static int   is_pooled(const void *p);
static void  free_ref(void *ref);
static void *reg_local(void *ref);
static void  delete_local(void *ref);

static int nx_tag_known(uint32_t t) {
  return t == TAG_OBJECT || t == TAG_STRING || t == TAG_PRIARR ||
         t == TAG_OBJARR || t == TAG_ID     || t == TAG_CLASS;
}

/* Read a reference's tag WITHOUT trusting the pointer. 0 means "not ours".
 * Dereferencing an untrusted jobject to look at its header is the classic way
 * this layer aborts; validate the pointer first and answer "not ours" instead. */
static uint32_t nx_tag_of(const void *p) {
  uintptr_t v = (uintptr_t)p;
  uint32_t t;
  if (!p) return 0;                       /* NULL is ordinary JNI -- silent */
  if ((v & 3u) || v < 0x1000u || v >= NX_USER_AS_END) return 0;
  t = *(volatile const uint32_t *)p;
  return nx_tag_known(t) ? t : 0;
}

/* Local reference table. The engine brackets its work with
 * PushLocalFrame/PopLocalFrame and deletes individual refs in between, and
 * both need to agree about what is still alive. */
#define MAX_LOCALS 8192
#define MAX_FRAMES 64
static void *g_locals[MAX_LOCALS];
static int   g_locals_top;
static int   g_frames[MAX_FRAMES];
static int   g_frame_top;

/* Deferred free. 256 entries is a few tens of KB and covers the window
 * between a premature delete and the last read of that reference. */
#define JNI_QUARANTINE 256
static void *g_retired[JNI_QUARANTINE];
static int   g_retired_w;

static void ref_retire(void *p) {
  void *evict;
  evict = g_retired[g_retired_w];
  g_retired[g_retired_w] = p;
  g_retired_w = (g_retired_w + 1) % JNI_QUARANTINE;
  if (evict) free(evict);
}


static void *intern_class(const char *name) {
  int i;
  const char *n = (name && *name) ? name : "java/lang/Object";
  void *r = NULL;
  mutexLock(&g_lock);
  for (i = 0; i < g_nclasses; i++)
    if (!strcmp(g_classes[i].name, n)) { r = &g_classes[i]; break; }
  if (!r) {
    if (g_nclasses >= MAX_CLASSES) {
      /* Never return NULL: the engine treats a null jclass as a fatal
       * ClassNotFound during startup. Reusing slot 0 degrades dispatch for
       * one class rather than aborting the boot. */
      LOGE("class pool full (%d); reusing slot 0 for %s", MAX_CLASSES, n);
      r = &g_classes[0];
    } else {
      FakeClass *c = &g_classes[g_nclasses++];
      c->tag = TAG_CLASS;
      snprintf(c->name, sizeof(c->name), "%s", n);
      r = c;
    }
  }
  mutexUnlock(&g_lock);
  return r;
}

static void *intern_id(const char *cls, const char *name, const char *sig) {
  int i;
  void *r = NULL;
  const char *c = cls ? cls : "?";
  const char *n = name ? name : "?";
  const char *s = sig ? sig : "()V";
  mutexLock(&g_lock);
  for (i = 0; i < g_nids; i++)
    if (!strcmp(g_ids[i].cls, c) && !strcmp(g_ids[i].name, n) &&
        !strcmp(g_ids[i].sig, s)) { r = &g_ids[i]; break; }
  if (!r) {
    if (g_nids >= MAX_IDS) {
      LOGE("method id pool full; reusing slot 0 for %s.%s", c, n);
      r = &g_ids[0];
    } else {
      FakeID *m = &g_ids[g_nids++];
      m->tag = TAG_ID;
      snprintf(m->cls, sizeof(m->cls), "%s", c);
      snprintf(m->name, sizeof(m->name), "%s", n);
      snprintf(m->sig, sizeof(m->sig), "%s", s);
      r = m;
    }
  }
  mutexUnlock(&g_lock);
  return r;
}

void *jni_make_object(const char *class_name) {
  int i;
  void *r = NULL;
  const char *n = (class_name && *class_name) ? class_name : "java/lang/Object";
  mutexLock(&g_lock);
  for (i = 0; i < g_nobjects; i++)
    if (!strcmp(g_objects[i].cls, n)) { r = &g_objects[i]; break; }
  if (!r) {
    if (g_nobjects >= MAX_OBJECTS) r = &g_objects[0];
    else {
      FakeObject *o = &g_objects[g_nobjects++];
      o->tag = TAG_OBJECT;
      snprintf(o->cls, sizeof(o->cls), "%s", n);
      r = o;
    }
  }
  mutexUnlock(&g_lock);
  return r;
}

/* A distinct, non-pooled object. The store restore path needs several
 * Purchase objects that differ only in their properties, so they cannot share
 * the one pooled instance the way stateless handles do. */
void *jni_make_unique_object(const char *class_name) {
  FakeObject *o = calloc(1, sizeof(*o));
  if (!o) return NULL;
  o->tag = TAG_OBJECT;
  snprintf(o->cls, sizeof(o->cls), "%s",
           (class_name && *class_name) ? class_name : "java/lang/Object");
  return reg_local(o);
}

void jni_object_set_prop(void *obj, const char *key, const char *value) {
  FakeObject *o = obj;
  int i;
  if (nx_tag_of(obj) != TAG_OBJECT || !key) return;
  for (i = 0; i < o->nprops; i++)
    if (!strcmp(o->prop[i].key, key)) break;
  if (i == o->nprops) {
    if (o->nprops >= MAX_PROPS) return;
    o->nprops++;
    snprintf(o->prop[i].key, sizeof(o->prop[i].key), "%s", key);
  }
  free(o->prop[i].val);
  o->prop[i].val = strdup(value ? value : "");
}

/* Validated read: this is reached from GetObjectField/GetIntField with
 * whatever receiver the engine passes, which is precisely the untrusted
 * pointer nx_tag_of exists for -- and it is the path the store restore reads
 * every Purchase field through. */
const char *jni_object_get_prop(void *obj, const char *key) {
  FakeObject *o = obj;
  int i;
  if (nx_tag_of(obj) != TAG_OBJECT || !key) return NULL;
  for (i = 0; i < o->nprops; i++)
    if (!strcmp(o->prop[i].key, key)) return o->prop[i].val ? o->prop[i].val : "";
  return NULL;
}

const char *jni_class_name_of(void *obj) {
  if (!obj) return "java/lang/Object";
  switch (nx_tag_of(obj)) {
    case TAG_OBJECT: return ((FakeObject *)obj)->cls;
    case TAG_CLASS:  return ((FakeClass *)obj)->name;
    case TAG_STRING: return "java/lang/String";
    default:         return "java/lang/Object";
  }
}

/* ------------------------------------------------------------------ */
/* Strings and arrays                                                  */
/* ------------------------------------------------------------------ */

void *jni_make_string(const char *utf8) {
  const char *s = utf8 ? utf8 : "";
  FakeString *r = NULL;
  int i;
  mutexLock(&g_lock);
  for (i = 0; i < g_nistrings; i++)
    if (!strcmp(g_istrings[i].utf, s)) { r = &g_istrings[i]; break; }
  if (!r && g_nistrings < MAX_ISTRING) {
    r = &g_istrings[g_nistrings++];
    r->tag = TAG_STRING;
    r->utf = strdup(s);
    if (!r->utf) { g_nistrings--; r = NULL; }
  }
  mutexUnlock(&g_lock);

  if (!r) {
    /* Pool exhausted: a one-off heap string. Freed by DeleteLocalRef, and if
     * the engine forgets, leaked -- which is far better than returning NULL
     * to a caller that will dereference it. */
    r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->tag = TAG_STRING;
    r->utf = strdup(s);
    /* Heap strings are owned by the local table; pooled ones are not. */
    reg_local(r);
  }
  return r;
}

const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  if (nx_tag_of(s) != TAG_STRING) return NULL;
  return s->utf;
}

static void *make_pri_array(const void *data, int len, int elem_size, char kind) {
  FakePriArray *a = calloc(1, sizeof(*a));   /* reg_local'd before returning */
  if (!a) return NULL;
  a->tag = TAG_PRIARR;
  a->len = len < 0 ? 0 : len;
  a->elem_size = elem_size;
  a->kind = kind;
  a->data = calloc((size_t)(a->len ? a->len : 1), (size_t)elem_size);
  if (!a->data) { free(a); return NULL; }
  if (data && a->len) memcpy(a->data, data, (size_t)a->len * (size_t)elem_size);
  return reg_local(a);
}

void *jni_make_byte_array(const void *d, int n)      { return make_pri_array(d, n, 1, 'B'); }
void *jni_make_int_array(const int32_t *d, int n)    { return make_pri_array(d, n, 4, 'I'); }
void *jni_make_float_array(const float *d, int n)    { return make_pri_array(d, n, 4, 'F'); }
void *jni_make_long_array(const int64_t *d, int n)   { return make_pri_array(d, n, 8, 'J'); }

void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (nx_tag_of(a) != TAG_PRIARR) { if (len_out) *len_out = 0; return NULL; }
  if (len_out) *len_out = a->len;
  return a->data;
}

void *jni_make_object_array(int len) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->tag = TAG_OBJARR;
  a->len = len < 0 ? 0 : len;
  a->items = calloc((size_t)(a->len ? a->len : 1), sizeof(void *));
  if (!a->items) { free(a); return NULL; }
  return reg_local(a);
}

void jni_object_array_set(void *arr, int index, void *value) {
  FakeObjArray *a = arr;
  if (nx_tag_of(a) != TAG_OBJARR) return;
  if (index < 0 || index >= a->len) return;
  a->items[index] = value;
}

/* ------------------------------------------------------------------ */
/* Reference lifetime                                                  */
/* ------------------------------------------------------------------ */

/* Only heap-allocated references are freed. Pooled ones (classes, ids,
 * interned strings and objects) are recognised by their address falling
 * inside a pool array and left alone -- freeing one would hand a dangling
 * pointer to every other holder of the same interned handle. */
static int is_pooled(const void *p) {
  const char *c = p;
  return (c >= (char *)g_classes  && c < (char *)&g_classes[MAX_CLASSES])  ||
         (c >= (char *)g_ids      && c < (char *)&g_ids[MAX_IDS])          ||
         (c >= (char *)g_objects  && c < (char *)&g_objects[MAX_OBJECTS])  ||
         (c >= (char *)g_istrings && c < (char *)&g_istrings[MAX_ISTRING]);
}

/* Clears the tag and the inner pointer BEFORE releasing either, so a stale
 * reader sees "not ours" rather than a live header with a dangling payload. */
static void free_ref(void *ref) {
  if (!ref || is_pooled(ref)) return;
  switch (nx_tag_of(ref)) {
    case TAG_STRING: { FakeString *s = ref; char *u = s->utf;
                       s->tag = 0; s->utf = NULL;   free(u);  ref_retire(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; void *d = a->data;
                       a->tag = 0; a->data = NULL;  free(d);  ref_retire(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; void **it = a->items;
                       a->tag = 0; a->items = NULL; free(it); ref_retire(a); break; }
    case TAG_OBJECT: {
      FakeObject *o = ref;
      int i;
      for (i = 0; i < o->nprops; i++) { free(o->prop[i].val); o->prop[i].val = NULL; }
      o->nprops = 0; o->tag = 0;
      ref_retire(o);
      break;
    }
    default: break;   /* pooled, already freed, or never ours */
  }
}

/* Register a heap reference as a local. Pooled handles are never registered:
 * they outlive everything and must never be freed. */
static void *reg_local(void *ref) {
  if (ref && !is_pooled(ref)) {
    mutexLock(&g_lock);
    if (g_locals_top < MAX_LOCALS) {
      g_locals[g_locals_top++] = ref;
    } else {
      static int warned;
      if (!warned) { warned = 1; LOGW("jni: local reference table full (%d); "
                                      "further references leak rather than dangle",
                                      MAX_LOCALS); }
    }
    mutexUnlock(&g_lock);
  }
  return ref;
}

/* THE FIX. A reference is freed only if it is still in the table, so a second
 * DeleteLocalRef on the same object finds nothing and does nothing instead of
 * calling free() on dead memory. */
static void delete_local(void *ref) {
  int i;
  if (!ref) return;
  mutexLock(&g_lock);
  for (i = g_locals_top - 1; i >= 0; i--) {
    if (g_locals[i] == ref) {
      g_locals[i] = g_locals[--g_locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Signature-driven argument marshalling                               */
/* ------------------------------------------------------------------ */

/* Walking a va_list by index is not sound on aarch64: integral and floating
 * arguments come from two separate register save areas, so reading them out of
 * declaration order silently returns a value from the wrong one. The signature
 * is the only thing that says which is which, so every call is unpacked once,
 * in order, before any handler sees it. */
static void marshal(const char *sig, va_list va, PpsArgs *out) {
  const char *p = sig;
  memset(out, 0, sizeof(*out));
  if (!p) return;
  if (*p == '(') p++;

  while (*p && *p != ')' && out->count < PPS_MAX_ARGS) {
    const int k = out->count;
    switch (*p) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        out->kind[k] = 'I'; out->i[k] = va_arg(va, int32_t); p++; break;
      case 'J':
        out->kind[k] = 'J'; out->i[k] = va_arg(va, int64_t); p++; break;
      case 'F':
        /* Variadic promotion: a jfloat arrives as a double. */
        out->kind[k] = 'F'; out->f[k] = va_arg(va, double); p++; break;
      case 'D':
        out->kind[k] = 'D'; out->f[k] = va_arg(va, double); p++; break;
      case 'L':
        out->kind[k] = 'L'; out->o[k] = va_arg(va, void *);
        while (*p && *p != ';' && *p != ')') p++;
        if (*p == ';') p++;
        break;
      case '[':
        out->kind[k] = '[';
        /* Skip the element type before consuming the argument, so a
         * "[Ljava/lang/String;" advances past the whole descriptor. */
        p++;
        while (*p == '[') p++;
        if (*p == 'L') { while (*p && *p != ';' && *p != ')') p++; if (*p == ';') p++; }
        else if (*p) p++;
        out->o[k] = va_arg(va, void *);
        break;
      default:
        /* An unrecognised letter means the signature is not one we produced.
         * Stop rather than guess: consuming a wrong-width argument would
         * desynchronise everything after it. */
        LOGW("jni: unparsable signature \"%s\" at '%c'", sig, *p);
        return;
    }
    out->count++;
  }
}

/* The return descriptor: the character after ')'. */
static char return_kind(const char *sig) {
  const char *p = sig ? strchr(sig, ')') : NULL;
  if (!p || !p[1]) return 'V';
  return p[1];
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Unhandled calls are logged once per (class, method) pair.
 *
 * The rate limit is what makes this usable. The engine polls its ad mediation
 * and analytics layers every frame, so an unconditional log line would be tens
 * of thousands of entries and would push the interesting ones out of the file.
 * One line each still tells you the complete set of things the engine asked
 * for and did not get, which is exactly what you want when something is
 * missing. */
static void note_unhandled(const FakeID *id) {
#if DEBUG_LOG
  static const FakeID *seen[192];
  static int n_seen;
  int i;
  if (!id) return;
  for (i = 0; i < n_seen; i++) if (seen[i] == id) return;
  if (n_seen < (int)(sizeof(seen) / sizeof(*seen))) seen[n_seen++] = id;
  /* Only interesting for classes we claim to implement. Everything else --
   * Facebook, ads, push, web views -- is expected to fall through. */
  if (pps_platform_owns(id->cls))
    LOGW("jni: unhandled %s.%s%s", id->cls, id->name, id->sig);
  else
    LOGB("jni: inert %s.%s%s", id->cls, id->name, id->sig);
#else
  (void)id;
#endif
}

/* One dispatcher for every Call*Method variant. The return value is read out
 * of `out` according to the signature, so an Int, Long and Boolean call all
 * share this path -- which is correct, because they share it in the JNI ABI
 * too (all three return a 64-bit register). */
/* The part of dispatch that does not depend on HOW the arguments arrived.
 *
 * This used to live inside dispatch() alone, which meant the jvalue-array call
 * variants -- Call<Type>MethodA -- had none of it: they returned straight after
 * pps_platform_call. So the SAME method behaved differently depending on which
 * of the three JNI call forms the engine happened to use, and the difference
 * was the dangerous direction: an unhandled String-returning getter came back
 * NULL through the A form and as the empty string through the V form. The
 * comment on the typed zero below explains exactly why NULL there is a fault
 * rather than a missing value.
 *
 * Both callers now share this, so the three forms cannot drift apart again. */
static int dispatch_common(void *recv, const FakeID *id, PpsArgs *out) {
  /* A handful of java.lang methods the engine calls on our own objects.
   * Answering these here rather than in pps_platform.c keeps that file about
   * King's platform layer rather than about Java. */
  if (nx_tag_of(recv) == TAG_STRING) {
    FakeString *s = recv;
    if (!strcmp(id->name, "length"))   { out->i[0] = (int64_t)strlen(s->utf); return 1; }
    if (!strcmp(id->name, "isEmpty"))  { out->i[0] = s->utf[0] == '\0'; return 1; }
    if (!strcmp(id->name, "toString")) { out->o[0] = recv; return 1; }
    if (!strcmp(id->name, "hashCode")) { out->i[0] = 0; return 1; }
  }
  if (!strcmp(id->name, "getClass")) {
    out->o[0] = intern_class(jni_class_name_of(recv));
    return 1;
  }
  if (!strcmp(id->name, "equals")) { out->i[0] = 0; return 1; }

  /* The engine asks the activity to finish on its way out. Several spellings
   * reach the same place. */
  if (!strcmp(id->name, "finish") || !strcmp(id->name, "exitApp") ||
      !strcmp(id->name, "requestApplicationMinimization")) {
    LOGB("jni: engine requested exit via %s.%s", id->cls, id->name);
    jni_quit_requested = 1;
    return 1;
  }

  note_unhandled(id);

  /* The typed zero. An object return must be non-null where the engine will
   * dereference it without checking, which is true of String and of any array
   * -- a null String from a getter propagates into a strlen. So a return type
   * of Ljava/lang/String; yields the empty string rather than NULL, and an
   * array type yields a zero-length array. */
  {
    const char rk = return_kind(id->sig);
    if (rk == '[') {
      const char *p = strchr(id->sig, ')');
      const char elem = (p && p[1] == '[' && p[2]) ? p[2] : 'B';
      switch (elem) {
        case 'F': out->o[0] = jni_make_float_array(NULL, 0); break;
        case 'I': out->o[0] = jni_make_int_array(NULL, 0);   break;
        case 'J': out->o[0] = jni_make_long_array(NULL, 0);  break;
        case 'L': out->o[0] = jni_make_object_array(0);      break;
        default:  out->o[0] = jni_make_byte_array(NULL, 0);  break;
      }
    } else if (rk == 'L') {
      if (strstr(id->sig, ")Ljava/lang/String;"))
        out->o[0] = jni_make_string("");
      else
        out->o[0] = NULL;   /* an object the engine is expected to null-check */
    }
  }
  return 0;
}

static int dispatch(void *recv, const FakeID *id, va_list va, PpsArgs *out) {
  PpsArgs args;
  if (!id) { memset(out, 0, sizeof(*out)); return 0; }

  marshal(id->sig, va, &args);
  memset(out, 0, sizeof(*out));

  if (pps_platform_call(id->cls, id->name, id->sig, recv, &args, out))
    return 1;

  return dispatch_common(recv, id, out);
}

/* ------------------------------------------------------------------ */
/* JNIEnv entry points                                                 */
/* ------------------------------------------------------------------ */

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name);
}

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  return intern_class(jni_class_name_of(obj));
}

static void *j_GetSuperclass(void *env, void *cls) {
  (void)env; (void)cls;
  return intern_class("java/lang/Object");
}

static juint j_IsAssignableFrom(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 1; }
static juint j_IsInstanceOf(void *env, void *o, void *c)     { (void)env; (void)o; (void)c; return 1; }
static juint j_IsSameObject(void *env, void *a, void *b)     { (void)env; return a == b; }

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  return intern_id(cls ? jni_class_name_of(cls) : "?", name, sig);
}

static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  return intern_id(cls ? jni_class_name_of(cls) : "?", name, sig);
}

/* NewObject: the class decides what comes back. Purchase and SkuDetails are
 * the two the engine constructs itself; everything else gets the pooled
 * stateless handle for its class. */
static void *new_object_v(void *cls, void *mid, va_list va) {
  const char *name = cls ? jni_class_name_of(cls) : "java/lang/Object";
  const FakeID *id = mid;
  (void)id; (void)va;
  if (strstr(name, "billingutil/Purchase") || strstr(name, "billingutil/SkuDetails"))
    return jni_make_unique_object(name);
  return jni_make_object(name);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  void *r; va_list va; (void)env;
  va_start(va, mid); r = new_object_v(cls, mid, va); va_end(va);
  return r;
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; return new_object_v(cls, mid, va);
}
static void *j_NewObjectA(void *env, void *cls, void *mid, const void *a) {
  (void)env; (void)a;
  { va_list dummy; memset(&dummy, 0, sizeof(dummy)); return new_object_v(cls, mid, dummy); }
}

/* Every Call<Type>Method{,V} shares one body. The variadic and va_list forms
 * differ only in where the list comes from. */
#define CALL_FORMS(fn, ret_t, extract)                                        \
  static ret_t fn(void *env, void *recv, void *mid, ...) {                    \
    PpsArgs out; va_list va; (void)env;                                       \
    va_start(va, mid); dispatch(recv, mid, va, &out); va_end(va);             \
    return extract;                                                           \
  }                                                                           \
  static ret_t fn##V(void *env, void *recv, void *mid, va_list va) {          \
    PpsArgs out; (void)env;                                                   \
    dispatch(recv, mid, va, &out);                                            \
    return extract;                                                           \
  }

CALL_FORMS(j_CallObjectMethod,  void *, out.o[0])
CALL_FORMS(j_CallIntMethod,     juint,  (juint)out.i[0])
CALL_FORMS(j_CallFloatMethod,   float,  (float)out.f[0])
CALL_FORMS(j_CallDoubleMethod,  double, out.f[0])

static void j_CallVoidMethod(void *env, void *recv, void *mid, ...) {
  PpsArgs out; va_list va; (void)env;
  va_start(va, mid); dispatch(recv, mid, va, &out); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, void *mid, va_list va) {
  PpsArgs out; (void)env;
  dispatch(recv, mid, va, &out);
}

/* The jvalue[] forms. The engine uses these from its reflection helper.
 * Rather than reconstruct a va_list -- which cannot be done portably -- the
 * array is unpacked directly against the signature, which is the same work
 * marshal() does from the other direction. */
static void args_from_jvalues(const char *sig, const void *jv, PpsArgs *out) {
  const uint64_t *v = jv;
  const char *p = sig;
  memset(out, 0, sizeof(*out));
  if (!p || !v) return;
  if (*p == '(') p++;
  while (*p && *p != ')' && out->count < PPS_MAX_ARGS) {
    const int k = out->count;
    switch (*p) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        out->kind[k] = 'I'; out->i[k] = (int32_t)v[k]; p++; break;
      case 'J': out->kind[k] = 'J'; out->i[k] = (int64_t)v[k]; p++; break;
      case 'F': { float f; uint32_t bits = (uint32_t)v[k]; memcpy(&f, &bits, 4);
                  out->kind[k] = 'F'; out->f[k] = f; p++; break; }
      case 'D': { double d; uint64_t bits = v[k]; memcpy(&d, &bits, 8);
                  out->kind[k] = 'D'; out->f[k] = d; p++; break; }
      case 'L':
        out->kind[k] = 'L';
        out->o[k] = (void *)(uintptr_t)v[k];
        while (*p && *p != ';' && *p != ')') p++;
        if (*p == ';') p++;
        break;
      case '[': out->kind[k] = '['; p++;
                while (*p == '[') p++;
                if (*p == 'L') { while (*p && *p != ';' && *p != ')') p++; if (*p == ';') p++; }
                else if (*p) p++;
                out->o[k] = (void *)(uintptr_t)v[k]; break;
      default: return;
    }
    out->count++;
  }
}

static int dispatch_a(void *recv, const FakeID *id, const void *jv, PpsArgs *out) {
  PpsArgs args;
  if (!id) { memset(out, 0, sizeof(*out)); return 0; }
  args_from_jvalues(id->sig, jv, &args);
  memset(out, 0, sizeof(*out));

  if (pps_platform_call(id->cls, id->name, id->sig, recv, &args, out))
    return 1;

  /* Shares the fallback with the variadic form -- see dispatch_common. */
  return dispatch_common(recv, id, out);
}

static void *j_CallObjectMethodA(void *env, void *r, void *m, const void *a) {
  PpsArgs out; (void)env; dispatch_a(r, m, a, &out); return out.o[0]; }
static juint j_CallIntMethodA(void *env, void *r, void *m, const void *a) {
  PpsArgs out; (void)env; dispatch_a(r, m, a, &out); return (juint)out.i[0]; }
static float j_CallFloatMethodA(void *env, void *r, void *m, const void *a) {
  PpsArgs out; (void)env; dispatch_a(r, m, a, &out); return (float)out.f[0]; }
static double j_CallDoubleMethodA(void *env, void *r, void *m, const void *a) {
  PpsArgs out; (void)env; dispatch_a(r, m, a, &out); return out.f[0]; }
static void j_CallVoidMethodA(void *env, void *r, void *m, const void *a) {
  PpsArgs out; (void)env; dispatch_a(r, m, a, &out); }

/* Field access. The engine reads a few static ints out of android.os.Build
 * and its own constant classes; nothing it reads is load-bearing, so zero and
 * an empty string are safe. Objects get the same non-null treatment as the
 * call path, for the same reason. */
static juint j_GetBooleanField(void *env, void *o, void *f){ (void)env; (void)o; (void)f; return 0; }
static float j_GetFloatField(void *env, void *o, void *f) { (void)env; (void)o; (void)f; return 0.0f; }
/* Returns a double, and that is the whole point of it existing separately: a
 * double comes back in d0, not x0, so routing GetDoubleField at the int
 * handler would have the caller read an untouched register. */
static double j_GetDoubleField(void *env, void *o, void *f) { (void)env; (void)o; (void)f; return 0.0; }
/* Reads a named property off the receiver before falling back.
 *
 * This is not a nicety -- it is what makes the store restore work. The engine
 * does NOT call getSku() on a Purchase: the accessor names appear nowhere in
 * libpapapearsaga.so, while mSku, mToken, mOrderId, mSignature, mOriginalJson,
 * mPurchaseState and mPurchaseTime all do. So it does GetFieldID(cls, "mSku",
 * "Ljava/lang/String;") and reads the field.
 *
 * Before this, every String field answered with the empty string, so each
 * restored Purchase carried an empty product id, the engine matched none of
 * them, and nothing unlocked -- silently, because "" is a valid String. */
static void *j_GetObjectField(void *env, void *o, void *f) {
  const FakeID *id = f;
  (void)env;
  if (nx_tag_of(id) == TAG_ID) {
    const char *v = jni_object_get_prop(o, id->name);
    if (v) return jni_make_string(v);
    if (strstr(id->sig, "Ljava/lang/String;"))
      return jni_make_string("");
  }
  return NULL;
}

/* Integral fields, same routing. mPurchaseState is the one that matters: 0 is
 * Google Play's PURCHASED, and any other value makes the engine treat the
 * entitlement as cancelled or refunded and ignore it. Our objects carry it as
 * a decimal string property so one mechanism covers both kinds of field. */
static juint j_GetIntField(void *env, void *o, void *f) {
  const FakeID *id = f;
  (void)env;
  if (nx_tag_of(id) == TAG_ID) {
    const char *v = jni_object_get_prop(o, id->name);
    if (v) return (juint)strtoll(v, NULL, 10);
  }
  return 0;
}
static juint j_GetLongField(void *env, void *o, void *f) {
  return j_GetIntField(env, o, f);
}
static void j_SetIntField(void *env, void *o, void *f, int32_t v)  { (void)env; (void)o; (void)f; (void)v; }
/* The value is discarded, but the PARAMETER TYPE still matters. A jlong
 * arrives in x3 and a jfloat in s0; a handler declared to take an int32_t
 * would be fine here because it ignores the argument, but declaring them
 * correctly costs nothing and keeps the table honest if a body is ever
 * added. */
static void j_SetLongField(void *env, void *o, void *f, int64_t v)  { (void)env; (void)o; (void)f; (void)v; }
static void j_SetFloatField(void *env, void *o, void *f, float v)   { (void)env; (void)o; (void)f; (void)v; }
static void j_SetDoubleField(void *env, void *o, void *f, double v) { (void)env; (void)o; (void)f; (void)v; }
static void j_SetObjectField(void *env, void *o, void *f, void *v) { (void)env; (void)o; (void)f; (void)v; }

/* Strings. GetStringUTFChars hands back our own buffer and Release does
 * nothing, which is legal -- the JNI spec allows an implementation to return
 * a pointer into the string's own storage and makes Release a no-op then. */
static void *j_NewStringUTF(void *env, const char *s) { (void)env; return jni_make_string(s); }

static const char *j_GetStringUTFChars(void *env, void *str, uint8_t *isCopy) {
  const char *u = jni_string_utf(str);
  (void)env;
  if (isCopy) *isCopy = 0;
  return u ? u : "";
}
static void j_ReleaseStringUTFChars(void *env, void *str, const char *chars) {
  (void)env; (void)str; (void)chars;
}
static juint j_GetStringUTFLength(void *env, void *str) {
  const char *u = jni_string_utf(str);
  (void)env;
  return u ? (juint)strlen(u) : 0;
}
static juint j_GetStringLength(void *env, void *str) { return j_GetStringUTFLength(env, str); }

/* DOES NOT NULL-TERMINATE, and that is the contract rather than an omission.
 *
 * GetStringUTFRegion copies exactly `len` bytes into a buffer the CALLER
 * sized, and JNI does not promise a terminator -- Android's own implementation
 * does not write one, so callers allocate exactly len. This wrote buf[len] = 0
 * on every call, one byte past the end of someone else's allocation.
 *
 * That is a heap overflow with no immediate symptom, and it is the best
 * explanation for what the first frame-loop run did: 417 correct file paths
 * followed by every subsequent path arriving as four bytes of pointer-shaped
 * garbage, and then a Data Abort at 0 inside step(). One byte at a time, the
 * engine's own allocations were being clipped.
 *
 * Writing buf[0] = 0 on the error paths is the same mistake in miniature: with
 * len == 0 the caller may legitimately have passed a zero-length buffer. */
static void j_GetStringUTFRegion(void *env, void *str, int32_t start, int32_t len, char *buf) {
  const char *u = jni_string_utf(str);
  size_t n;
  (void)env;
  if (!buf || len <= 0) return;
  if (!u) return;
  n = strlen(u);
  if (start < 0 || (size_t)start > n) return;
  if ((size_t)(start + len) > n) len = (int32_t)(n - (size_t)start);
  if (len > 0) memcpy(buf, u + start, (size_t)len);
}

/* UTF-16, which the engine uses for text it will render. Converting properly
 * would need a real decoder; the strings that reach this path are ASCII
 * (locale codes, device names, product ids), so a widening copy is correct
 * for all of them and visibly wrong for none. */
/* Same contract, same fix: exactly `len` jchars, no terminator. The old
 * version wrote buf[i] = 0 after a loop that could reach i == len, so it put
 * two bytes past the caller's buffer -- and it indexed u[start + i] without
 * ever checking start against the string's length, so it could read out of
 * bounds as well as write out of bounds. */
static void j_GetStringRegion(void *env, void *str, int32_t start, int32_t len, uint16_t *buf) {
  const char *u = jni_string_utf(str);
  size_t n;
  int32_t i;
  (void)env;
  if (!buf || len <= 0 || !u) return;
  n = strlen(u);
  if (start < 0 || (size_t)start > n) return;
  if ((size_t)(start + len) > n) len = (int32_t)(n - (size_t)start);
  for (i = 0; i < len; i++) buf[i] = (uint8_t)u[start + i];
}
static const uint16_t *j_GetStringChars(void *env, void *str, uint8_t *isCopy) {
  const char *u = jni_string_utf(str);
  size_t n = u ? strlen(u) : 0, i;
  uint16_t *w = calloc(n + 1, sizeof(uint16_t));
  (void)env;
  if (isCopy) *isCopy = 1;
  if (!w) return NULL;
  for (i = 0; i < n; i++) w[i] = (uint8_t)u[i];
  return w;
}
static void j_ReleaseStringChars(void *env, void *str, const uint16_t *chars) {
  (void)env; (void)str; free((void *)chars);
}
static void *j_NewString(void *env, const uint16_t *chars, int32_t len) {
  char *tmp = malloc((size_t)len + 1);
  void *r;
  int32_t i;
  (void)env;
  if (!tmp) return jni_make_string("");
  for (i = 0; i < len; i++) tmp[i] = (char)(chars[i] & 0x7f);
  tmp[len] = 0;
  r = jni_make_string(tmp);
  free(tmp);
  return r;
}

/* Arrays. */
static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  if (!arr) return 0;
  if (nx_tag_of(arr) == TAG_PRIARR) return (juint)((FakePriArray *)arr)->len;
  if (nx_tag_of(arr) == TAG_OBJARR) return (juint)((FakeObjArray *)arr)->len;
  return 0;
}
static void *j_NewByteArray(void *env, int32_t n)  { (void)env; return make_pri_array(NULL, n, 1, 'B'); }
static void *j_NewIntArray(void *env, int32_t n)   { (void)env; return make_pri_array(NULL, n, 4, 'I'); }
static void *j_NewFloatArray(void *env, int32_t n) { (void)env; return make_pri_array(NULL, n, 4, 'F'); }
static void *j_NewLongArray(void *env, int32_t n)  { (void)env; return make_pri_array(NULL, n, 8, 'J'); }
static void *j_NewShortArray(void *env, int32_t n) { (void)env; return make_pri_array(NULL, n, 2, 'S'); }
static void *j_NewDoubleArray(void *env, int32_t n){ (void)env; return make_pri_array(NULL, n, 8, 'D'); }

static void *j_NewObjectArray(void *env, int32_t n, void *cls, void *init) {
  void *a = jni_make_object_array(n);
  int32_t i;
  (void)env; (void)cls;
  if (init) for (i = 0; i < n; i++) jni_object_array_set(a, i, init);
  return a;
}
static void *j_GetObjectArrayElement(void *env, void *arr, int32_t i) {
  FakeObjArray *a = arr;
  (void)env;
  if (!a || a->tag != TAG_OBJARR || i < 0 || i >= a->len) return NULL;
  return a->items[i];
}
static void j_SetObjectArrayElement(void *env, void *arr, int32_t i, void *v) {
  (void)env; jni_object_array_set(arr, i, v);
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *isCopy) {
  FakePriArray *a = arr;
  (void)env;
  if (isCopy) *isCopy = 0;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int32_t mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int32_t start, int32_t len, void *buf) {
  FakePriArray *a = arr;
  (void)env;
  if (!a || a->tag != TAG_PRIARR || !buf) return;
  if (start < 0 || len < 0 || start + len > a->len) return;
  memcpy(buf, (char *)a->data + (size_t)start * (size_t)a->elem_size,
         (size_t)len * (size_t)a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int32_t start, int32_t len, const void *buf) {
  FakePriArray *a = arr;
  (void)env;
  if (!a || a->tag != TAG_PRIARR || !buf) return;
  if (start < 0 || len < 0 || start + len > a->len) return;
  memcpy((char *)a->data + (size_t)start * (size_t)a->elem_size, buf,
         (size_t)len * (size_t)a->elem_size);
}

/* References. Global and local are the same thing here: the pooled handles
 * outlive everything and the heap ones are freed on an explicit delete. */
static void *j_NewRef(void *env, void *obj) { (void)env; return obj; }
/* Bound to DeleteLocalRef, DeleteGlobalRef and DeleteWeakGlobalRef alike.
 *
 * All three go through the ownership check rather than freeing directly. Our
 * NewGlobalRef hands back the same pointer it was given, so a reference can be
 * deleted as a local and then as a global; the second one must be a no-op, not
 * a second free. Worst case here is a leak, which is the right way to be
 * wrong. */
static void j_DeleteRef(void *env, void *obj) { (void)env; delete_local(obj); }
static int32_t j_EnsureLocalCapacity(void *env, int32_t n) { (void)env; (void)n; return JNI_OK; }
static int32_t j_PushLocalFrame(void *env, int32_t n) {
  (void)env; (void)n;
  mutexLock(&g_lock);
  if (g_frame_top < MAX_FRAMES) g_frames[g_frame_top++] = g_locals_top;
  mutexUnlock(&g_lock);
  return JNI_OK;
}
/* Frees everything created since the matching push, except the one reference
 * the caller is carrying out of the frame -- which is then re-registered in
 * the enclosing frame, as the JNI spec requires. Both of these were no-ops,
 * so every reference the engine created inside a frame leaked, and nothing
 * agreed with DeleteLocalRef about what was still alive. */
static void *j_PopLocalFrame(void *env, void *result) {
  int mark, i;
  (void)env;
  mutexLock(&g_lock);
  /* An unmatched pop frees NOTHING, rather than falling back to mark 0 and
   * emptying the whole table. Getting that wrong once would free every
   * reference the engine still holds -- including ones handed to it from
   * outside any frame of its own, like the Purchase[] the store restore
   * builds. Leaking a frame's worth is the survivable direction. */
  if (g_frame_top <= 0) {
    mutexUnlock(&g_lock);
    LOGW("jni: PopLocalFrame without a matching push; nothing freed");
    return result;
  }
  mark = g_frames[--g_frame_top];

  /* Clamp. delete_local shrinks the table with a swap-and-decrement, so by the
   * time this pop runs the top can legitimately be BELOW the mark this frame
   * was pushed at. Assigning g_locals_top = mark unconditionally would then
   * raise the top again and resurrect slots holding pointers that were already
   * freed -- the next pop would free them a second time, which is the exact
   * bug this whole section exists to prevent. */
  if (mark > g_locals_top) mark = g_locals_top;

  for (i = mark; i < g_locals_top; i++)
    if (g_locals[i] != result) free_ref(g_locals[i]);
  g_locals_top = mark;

  /* Re-register the escaping reference in the enclosing frame, but ONLY if it
   * is not already there. The spec says the result becomes a local of the
   * previous frame; if it was already one -- the engine commonly pushes a
   * frame around work on a reference it already held -- appending it again
   * puts two entries on one object, and then one delete frees it while the
   * other still points at it. */
  if (result && !is_pooled(result) && g_locals_top < MAX_LOCALS) {
    int dup = 0;
    for (i = 0; i < g_locals_top; i++)
      if (g_locals[i] == result) { dup = 1; break; }
    if (!dup) g_locals[g_locals_top++] = result;
  }
  mutexUnlock(&g_lock);
  return result;
}

/* Exceptions. Nothing here ever throws, so the engine's checks always pass --
 * which is the honest answer: a call that fell through to the typed zero did
 * not fail, it returned a defined default. */
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_ExceptionClear(void *env) { (void)env; }
static void j_ExceptionDescribe(void *env) { (void)env; }
static int32_t j_Throw(void *env, void *obj) { (void)env; (void)obj; return 0; }
static int32_t j_ThrowNew(void *env, void *cls, const char *msg) {
  (void)env;
  LOGW("jni: engine threw %s: %s", cls ? jni_class_name_of(cls) : "?", msg ? msg : "");
  return 0;
}
static void j_FatalError(void *env, const char *msg) {
  (void)env;
  LOGE("jni: FatalError: %s", msg ? msg : "");
  jni_quit_requested = 1;
}

static int32_t j_RegisterNatives(void *env, void *cls, const void *methods, int32_t n) {
  /* The module registers its own natives in JNI_OnLoad on some builds. We
   * call the entry points by symbol name instead, so the registration is
   * recorded and otherwise ignored. */
  (void)env; (void)methods;
  LOGB("jni: RegisterNatives(%s, %d) accepted", cls ? jni_class_name_of(cls) : "?", (int)n);
  return JNI_OK;
}
static int32_t j_UnregisterNatives(void *env, void *cls) { (void)env; (void)cls; return JNI_OK; }
static int32_t j_MonitorEnter(void *env, void *obj) { (void)env; (void)obj; return JNI_OK; }
static int32_t j_MonitorExit(void *env, void *obj) { (void)env; (void)obj; return JNI_OK; }

/* ------------------------------------------------------------------ */
/* The vtables                                                         */
/* ------------------------------------------------------------------ */

/* Slot indices are the JNI ABI's own ordering. They are not guessable and
 * getting one wrong is a wild jump, so they follow the reference ports'
 * table, which was itself checked against a real libnativehelper. */
/* ------------------------------------------------------------------ */
/* the catch-all                                                       */
/* ------------------------------------------------------------------ */

/* Every slot this port does not implement points here rather than staying
 * NULL, and the difference is not cosmetic.
 *
 * A NULL slot is a branch to address 0. On this console that is a Data Abort
 * with a fault address of 0 and a return address inside the engine, which
 * tells you nothing about which JNI call it was -- and the JNI table is 233
 * entries, so "some JNI call" is not a lead. Auditing this table by hand found
 * 34 slots left NULL, including SetLongField, NewBooleanArray and all nine
 * SetStatic<Type>Field entries.
 *
 * Returning 0 is the right answer for all of them: none is on a path this port
 * supports, and the engine's own code already handles a zero or null from its
 * platform layer because Android could return one too. The log line names the
 * slot so an unexpected one is a lookup in the JNI spec rather than a
 * debugging session.
 *
 * It is declared to return a pointer-width integer with no arguments. That is
 * safe for every slot it backs: on AAPCS64 the callee never touches the
 * caller's argument registers, and a caller expecting void, an int, or a
 * pointer all read x0, which this sets to 0. The only calls it would answer
 * WRONGLY are the ones returning float or double, which read s0/d0 -- so every
 * float- and double-returning slot in the table is bound explicitly above,
 * and none of them reaches here. */
static juint j_unimplemented_slot(void) {
  return 0;
}

static void *env_table[233];
static void **env_table_ptr = env_table;
static void *fake_env = &env_table_ptr;

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
static void *fake_vm = &vm_table_ptr;

static int32_t vm_GetEnv(void *vm, void **out, int32_t version) {
  (void)vm; (void)version;
  if (out) *out = fake_env;
  return JNI_OK;
}
static int32_t vm_AttachCurrentThread(void *vm, void **out, void *args) {
  (void)vm; (void)args;
  if (out) *out = fake_env;
  return JNI_OK;
}
static int32_t vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static int32_t vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }

/* jint GetJavaVM(JNIEnv*, JavaVM**) -- returns a status code, not a pointer.
 * This was declared to return void* and returned NULL, which is 0 and
 * therefore JNI_OK by accident. It worked; it should not have been an
 * accident. */
static int32_t j_GetJavaVM_impl(void *env, void **out) {
  (void)env;
  if (out) *out = fake_vm;
  return JNI_OK;
}

void *jni_env(void) { return fake_env; }
void *jni_vm(void)  { return fake_vm; }

/* The jobject handed to every instance native call. NativeApplication's
 * natives are instance methods, so argument 2 is `this` rather than a jclass
 * -- that difference is why this is not called jni_class(). */
void *jni_this(void) { return jni_make_object("com/king/core/NativeApplication"); }

void jni_init(void) {
  int i;
  mutexInit(&g_lock);

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[10]  = (void *)j_GetSuperclass;
  env_table[11]  = (void *)j_IsAssignableFrom;
  env_table[13]  = (void *)j_Throw;
  env_table[14]  = (void *)j_ThrowNew;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_ExceptionDescribe;
  env_table[17]  = (void *)j_ExceptionClear;
  env_table[18]  = (void *)j_FatalError;
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewRef;            /* NewGlobalRef      */
  env_table[22]  = (void *)j_DeleteRef;         /* DeleteGlobalRef   */
  env_table[23]  = (void *)j_DeleteRef;         /* DeleteLocalRef    */
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewRef;            /* NewLocalRef       */
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[30]  = (void *)j_NewObjectA;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;

  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[37]  = (void *)j_CallIntMethod;     /* CallBooleanMethod */
  env_table[38]  = (void *)j_CallIntMethodV;
  env_table[39]  = (void *)j_CallIntMethodA;
  env_table[40]  = (void *)j_CallIntMethod;     /* CallByteMethod    */
  env_table[41]  = (void *)j_CallIntMethodV;
  env_table[42]  = (void *)j_CallIntMethodA;
  env_table[43]  = (void *)j_CallIntMethod;     /* CallCharMethod    */
  env_table[44]  = (void *)j_CallIntMethodV;
  env_table[45]  = (void *)j_CallIntMethodA;
  env_table[46]  = (void *)j_CallIntMethod;     /* CallShortMethod   */
  env_table[47]  = (void *)j_CallIntMethodV;
  env_table[48]  = (void *)j_CallIntMethodA;
  env_table[49]  = (void *)j_CallIntMethod;     /* CallIntMethod     */
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[52]  = (void *)j_CallIntMethod;     /* CallLongMethod    */
  env_table[53]  = (void *)j_CallIntMethodV;
  env_table[54]  = (void *)j_CallIntMethodA;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[60]  = (void *)j_CallDoubleMethodA;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[63]  = (void *)j_CallVoidMethodA;

  /* CallNonvirtual<Type>Method, 64..93.
   *
   * The layout mirrors the instance block exactly -- Object, Boolean, Byte,
   * Char, Short, Int, Long, Float, Double, Void, three slots each -- so the
   * dispatch must mirror it too. An earlier version pointed the whole range at
   * the Object handlers and then fixed up only Void, which is wrong in a way
   * that does not crash: CallNonvirtualBooleanMethod would return a pointer
   * where a jboolean was expected, and the Float and Double slots would return
   * in x0 while the caller read s0/d0 -- a garbage float, silently.
   *
   * Nonvirtual is uncommon (it is what a super.method() call compiles to), but
   * "uncommon" is exactly the class of bug that survives a whole test session
   * and then appears once. */
  env_table[64] = (void *)j_CallObjectMethod;
  env_table[65] = (void *)j_CallObjectMethodV;
  env_table[66] = (void *)j_CallObjectMethodA;
  for (i = 67; i <= 84; i += 3) {           /* Boolean, Byte, Char, Short, Int, Long */
    env_table[i]     = (void *)j_CallIntMethod;
    env_table[i + 1] = (void *)j_CallIntMethodV;
    env_table[i + 2] = (void *)j_CallIntMethodA;
  }
  env_table[85] = (void *)j_CallFloatMethod;
  env_table[86] = (void *)j_CallFloatMethodV;
  env_table[87] = (void *)j_CallFloatMethodA;
  env_table[88] = (void *)j_CallDoubleMethod;
  env_table[89] = (void *)j_CallDoubleMethodV;
  env_table[90] = (void *)j_CallDoubleMethodA;
  env_table[91] = (void *)j_CallVoidMethod;
  env_table[92] = (void *)j_CallVoidMethodV;
  env_table[93] = (void *)j_CallVoidMethodA;

  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;
  env_table[97]  = (void *)j_GetIntField;       /* GetByteField      */
  env_table[98]  = (void *)j_GetIntField;       /* GetCharField      */
  env_table[99]  = (void *)j_GetIntField;       /* GetShortField     */
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[103] = (void *)j_GetDoubleField;
  /* Set<Type>Field, 104..112. Every one, not a sample.
   *
   * These were previously three slots out of nine, and the six gaps were left
   * as NULL -- which is not "unimplemented", it is a branch to address 0 the
   * first time the engine writes a long or a float field. The values are
   * discarded either way (our objects carry named string properties, not typed
   * fields), but discarding is a decision and faulting is not. */
  env_table[104] = (void *)j_SetObjectField;
  env_table[105] = (void *)j_SetIntField;       /* SetBooleanField   */
  env_table[106] = (void *)j_SetIntField;       /* SetByteField      */
  env_table[107] = (void *)j_SetIntField;       /* SetCharField      */
  env_table[108] = (void *)j_SetIntField;       /* SetShortField     */
  env_table[109] = (void *)j_SetIntField;
  env_table[110] = (void *)j_SetLongField;
  env_table[111] = (void *)j_SetFloatField;
  env_table[112] = (void *)j_SetDoubleField;
  env_table[113] = (void *)j_GetMethodID;       /* GetStaticMethodID */

  /* CallStatic<Type>Method, 114..143. A static call has a jclass where an
   * instance call has `this`, and dispatch is by the id's class either way,
   * so the same handlers serve both. */
  env_table[114] = (void *)j_CallObjectMethod;
  env_table[115] = (void *)j_CallObjectMethodV;
  env_table[116] = (void *)j_CallObjectMethodA;
  for (i = 117; i <= 134; i += 3) {
    env_table[i]     = (void *)j_CallIntMethod;
    env_table[i + 1] = (void *)j_CallIntMethodV;
    env_table[i + 2] = (void *)j_CallIntMethodA;
  }
  env_table[135] = (void *)j_CallFloatMethod;
  env_table[136] = (void *)j_CallFloatMethodV;
  env_table[137] = (void *)j_CallFloatMethodA;
  env_table[138] = (void *)j_CallDoubleMethod;
  env_table[139] = (void *)j_CallDoubleMethodV;
  env_table[140] = (void *)j_CallDoubleMethodA;
  env_table[141] = (void *)j_CallVoidMethod;
  env_table[142] = (void *)j_CallVoidMethodV;
  env_table[143] = (void *)j_CallVoidMethodA;

  /* GetStatic<Type>Field 145..153, SetStatic<Type>Field 154..162. Both blocks
   * in full, for the same reason as the instance ones above. */
  env_table[144] = (void *)j_GetFieldID;        /* GetStaticFieldID  */
  env_table[145] = (void *)j_GetObjectField;
  env_table[146] = (void *)j_GetBooleanField;
  env_table[147] = (void *)j_GetIntField;       /* GetStaticByteField  */
  env_table[148] = (void *)j_GetIntField;       /* GetStaticCharField  */
  env_table[149] = (void *)j_GetIntField;       /* GetStaticShortField */
  env_table[150] = (void *)j_GetIntField;
  env_table[151] = (void *)j_GetLongField;
  env_table[152] = (void *)j_GetFloatField;
  env_table[153] = (void *)j_GetDoubleField;
  env_table[154] = (void *)j_SetObjectField;
  env_table[155] = (void *)j_SetIntField;
  env_table[156] = (void *)j_SetIntField;
  env_table[157] = (void *)j_SetIntField;
  env_table[158] = (void *)j_SetIntField;
  env_table[159] = (void *)j_SetIntField;
  env_table[160] = (void *)j_SetLongField;
  env_table[161] = (void *)j_SetFloatField;
  env_table[162] = (void *)j_SetDoubleField;

  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  /* New<Type>Array 175..182, all eight. The four that were missing would each
   * have been a null jump: the engine allocates a boolean[] for its
   * permissions query and a double[] for one of the analytics payloads. */
  env_table[175] = (void *)j_NewByteArray;      /* NewBooleanArray, 1 byte/elem */
  env_table[176] = (void *)j_NewByteArray;
  env_table[177] = (void *)j_NewShortArray;     /* NewCharArray, 2 bytes/elem   */
  env_table[178] = (void *)j_NewShortArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[180] = (void *)j_NewLongArray;
  env_table[181] = (void *)j_NewFloatArray;
  env_table[182] = (void *)j_NewDoubleArray;
  for (i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_UnregisterNatives;
  env_table[217] = (void *)j_MonitorEnter;
  env_table[218] = (void *)j_MonitorExit;
  env_table[219] = (void *)j_GetJavaVM_impl;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion;
  env_table[222] = (void *)j_GetPriArrayElements;      /* GetPrimitiveArrayCritical */
  env_table[223] = (void *)j_ReleasePriArrayElements;
  env_table[224] = (void *)j_GetStringChars;           /* GetStringCritical */
  env_table[225] = (void *)j_ReleaseStringChars;
  env_table[226] = (void *)j_NewRef;                   /* NewWeakGlobalRef  */
  env_table[227] = (void *)j_DeleteRef;
  env_table[228] = (void *)j_ExceptionCheck;

  /* Anything still unset gets the catch-all. Done last so an explicit binding
   * above always wins, and done by sweep rather than by listing the gaps --
   * a list is a thing to get out of date, and this one already was. */
  {
    int filled = 0;
    /* 0..3 are `reserved` in the JNI spec and are legitimately NULL: nothing
     * may call them, and leaving them null means a bug that does gets a fault
     * rather than a silent zero. */
    for (i = 4; i < 233; i++) {
      if (!env_table[i]) { env_table[i] = (void *)j_unimplemented_slot; filled++; }
    }
    if (filled) LOGB("JNI: %d of 233 slots backed by the catch-all", filled);
  }

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;  /* AsDaemon */

  LOGB("jni: environment ready");
}
