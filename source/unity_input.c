/* unity_input.c -- fake MotionEvent / KeyEvent backing nativeInjectEvent.
 * See unity_input.h for the method surface (taken from libunity.so) and wiring.
 * Style mirrors unity_jni.c. Host-compilable plain C (no libnx). */

#include <string.h>
#include <time.h>
#include "unity_input.h"
#include "jni_fake.h"   /* jni_make_string / int array / array length */

struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

enum { UI_TAG = 0x55494531 /*'UIE1'*/, KIND_MOTION, KIND_KEY };

#define UI_MAX_AXES 32           /* Android AXIS_* ids used here top out at 18 */
typedef struct {
  uint32_t tag; int kind;
  int   action;                 /* raw action (masked | ptrindex<<8)        */
  int   count;
  int   ids[UI_MAX_POINTERS];
  float xs [UI_MAX_POINTERS];
  float ys [UI_MAX_POINTERS];
  int   keycode;                /* KeyEvent                                 */
  int64_t time_ms;
  int   source;                 /* AINPUT_SOURCE_*: touchscreen / gamepad / joystick */
  int   device_id;              /* 0 = touchscreen, 1 = gamepad             */
  float axes[UI_MAX_AXES];      /* joystick MotionEvent: getAxisValue(axis) */
} UEvent;

/* ---- InputDevice model -------------------------------------------------
 * Two devices, as Android would enumerate on a phone with a Pro Controller
 * paired: the built-in touchscreen (id 0) and the pad (id 1). Rewired's
 * Android backend calls InputDevice.getDeviceIds() and then getDevice(id) on
 * each, keying its controller definition on vendor/product; with these
 * approximated to an opaque object it saw no controller at all (boot 22).
 * Values are what Android reports for a real Nintendo Switch Pro Controller:
 * vendor 0x057e, product 0x2009, sources GAMEPAD|JOYSTICK|DPAD, digital
 * triggers, dpad as HAT_X/HAT_Y axes. */
enum { UID_TAG = 0x55494432 /*'UID2'*/, UMR_TAG = 0x554d5231 /*'UMR1'*/, ULS_TAG = 0x554c5331 /*'ULS1'*/ };
typedef struct { uint32_t tag; int id; } UDevice;
typedef struct { uint32_t tag; int axis; int source; float lo, hi, flat, fuzz; } UMotionRange;
typedef struct { uint32_t tag; int n; void *items[16]; } UList;
static UDevice g_dev_touch = { UID_TAG, 0 };
static UDevice g_dev_pad   = { UID_TAG, 1 };
#define AINPUT_SOURCE_DPAD      0x00000201
#define AINPUT_SOURCE_GAMEPAD   0x00000401
#define AINPUT_SOURCE_JOYSTICK  0x01000010
#define NX_PAD_SOURCES (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK | AINPUT_SOURCE_DPAD)
/* Android AXIS_* */
enum { AXIS_X=0, AXIS_Y=1, AXIS_Z=11, AXIS_RZ=14, AXIS_HAT_X=15, AXIS_HAT_Y=16, AXIS_LTRIGGER=17, AXIS_RTRIGGER=18 };
static const int g_pad_axes[] = { AXIS_X, AXIS_Y, AXIS_Z, AXIS_RZ, AXIS_HAT_X, AXIS_HAT_Y };
static UMotionRange g_pad_ranges[6];
static UList        g_pad_range_list;
static void pad_ranges_init(void){
  if (g_pad_range_list.tag) return;
  g_pad_range_list.tag = ULS_TAG; g_pad_range_list.n = 0;
  for (unsigned i = 0; i < sizeof g_pad_axes / sizeof g_pad_axes[0]; i++) {
    UMotionRange *r = &g_pad_ranges[i];
    r->tag = UMR_TAG; r->axis = g_pad_axes[i];
    r->source = (r->axis == AXIS_HAT_X || r->axis == AXIS_HAT_Y) ? AINPUT_SOURCE_DPAD : AINPUT_SOURCE_JOYSTICK;
    r->lo = -1.0f; r->hi = 1.0f; r->flat = 0.05f; r->fuzz = 0.0f;
    g_pad_range_list.items[g_pad_range_list.n++] = r;
  }
}
void *unity_inputdevice(int id){ return id == 1 ? (void *)&g_dev_pad : (void *)&g_dev_touch; }
int   input_is_device(const void *p){ const UDevice *d = p; return d && d->tag == UID_TAG; }


/* single reused handle -- injection is synchronous */
static UEvent g_ev;

/* ---- touch diagnostics (see unity_input.h) ---- */
int (*input_log_fn)(char *fmt, ...) = 0;
int   input_log_budget = 0;
#define ILOG(...) do { if (input_log_fn && input_log_budget > 0) { \
                         input_log_budget--; input_log_fn(__VA_ARGS__); } } while (0)

static int64_t now_ms(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
static int  has(const char *s,const char *sub){ return strstr(s,sub)!=NULL; }
static int  is_ev(void *p,int kind) __attribute__((unused));
static int  is_ev(void *p,int kind){ UEvent*e=p; return e && e->tag==UI_TAG && e->kind==kind; }

/* ---- constructors ------------------------------------------------------- */
void *unity_motionevent(int action,int count,const int *ids,const float *xs,const float *ys){
  UEvent *e=&g_ev; memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_MOTION; e->action=action; e->time_ms=now_ms();
  e->source=AINPUT_SOURCE_TOUCHSCREEN; e->device_id=0;
  if (count>UI_MAX_POINTERS) count=UI_MAX_POINTERS;
  e->count=count;
  for (int i=0;i<count;i++){ e->ids[i]=ids?ids[i]:i; e->xs[i]=xs?xs[i]:0; e->ys[i]=ys?ys[i]:0; }
  return e;
}
void *unity_keyevent(int action,int keycode){
  UEvent *e=&g_ev; memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_KEY; e->action=action; e->keycode=keycode; e->time_ms=now_ms();
  e->source=AINPUT_SOURCE_KEYBOARD; e->device_id=0;
  return e;
}
/* Gamepad button: KeyEvent from device 1 with the sources a real pad reports
 * (Android sets GAMEPAD|KEYBOARD on pad key events). */
void *unity_keyevent_pad(int action,int keycode){
  UEvent *e=&g_ev; memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_KEY; e->action=action; e->keycode=keycode; e->time_ms=now_ms();
  e->source=AINPUT_SOURCE_GAMEPAD|AINPUT_SOURCE_KEYBOARD; e->device_id=1;
  return e;
}
/* Joystick motion: one MotionEvent (ACTION_MOVE, one "pointer") from device 1
 * carrying every axis; Unity reads them with getAxisValue(axis). */
void *unity_joystick_event(const float *axes, int naxes){
  UEvent *e=&g_ev; memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_MOTION; e->action=AMOTION_ACTION_MOVE; e->time_ms=now_ms();
  e->count=1; e->ids[0]=0;
  e->source=AINPUT_SOURCE_JOYSTICK; e->device_id=1;
  for (int i=0;i<naxes && i<UI_MAX_AXES;i++) e->axes[i]=axes[i];
  return e;
}

/* MotionEvent.obtain(MotionEvent src): Android's copy factory. nativeInjectEvent
 * copies our injected event into one IT owns and reads that copy *after* inject
 * returns (across frames), so we must hand back a real, separate UEvent copy --
 * not g_ev, which the next frame overwrites. A small ring keeps several in-flight
 * copies alive until the engine finishes reading them. */
/* Sized 32, not 16: the pointer layer can now emit several events in one frame
 * (a POINTER_DOWN, a batched MOVE and a POINTER_UP can all land together with
 * multitouch), so a 16-slot ring would recycle a copy after only four or five
 * frames instead of sixteen. 32 UEvents is a few KB. */
static UEvent   g_ev_copies[32];
static unsigned g_ev_copy_i;
void *unity_motionevent_obtain(void *src){
  UEvent *s = src;
  if (!s || s->tag!=UI_TAG) return src;          /* not ours -> passthrough     */
  UEvent *d = &g_ev_copies[g_ev_copy_i++ & 31];
  *d = *s;
  return d;
}

/* ---- ownership ---------------------------------------------------------- */
int input_owns_class(const char *cls){
  return has(cls,"view/MotionEvent") || has(cls,"view/KeyEvent") ||
         has(cls,"view/InputEvent") || has(cls,"view/InputDevice");
}
/* Route by receiver, not class name: GetObjectClass() on our event handle
 * reports java/lang/Object (jni_fake only special-cases Bitmap), so class-name
 * routing misses every getter the engine resolves via GetObjectClass(event).
 * The tag is unique to our UEvent handle, so this is exact. */
int input_owns_recv(const void *recv){
  /* our event handle, OR an InputDevice / MotionRange / List from the device
   * model (boot 22): all are tagged in their first word. */
  const uint32_t *t = recv;
  if (!recv || ((uintptr_t)recv & 3)) return 0;
  return *t==UI_TAG || *t==UID_TAG || *t==UMR_TAG || *t==ULS_TAG;
}
/* For instanceof classification by nativeInjectEvent: true if our handle is a
 * MotionEvent (vs KeyEvent). Caller must have checked input_owns_recv first. */
int input_recv_is_motion(const void *recv){
  const UEvent *e = recv; return e && e->tag==UI_TAG && e->kind==KIND_MOTION;
}

/* getX/getY/getPressure/... come as ()F or (I)F -- pull the pointer index when
 * the signature carries one. */
static int ptr_index(const struct FakeID *id, va_list va){
  if (strstr(id->sig,"(I)")) { int idx=va_arg(va,int); return idx; }
  return 0;
}

/* ---- int / long getters ------------------------------------------------- */
uint64_t input_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  ILOG("    [in.i] %s  (cls=%s)\n", m, id->cls);

  /* ---- InputDevice ---- */
  if (input_is_device(recv)) {
    const UDevice *d = recv; const int pad = d->id == 1;
    if (has(m,"getId"))               return (uint64_t)d->id;
    if (has(m,"getSources"))          return pad ? NX_PAD_SOURCES : (uint64_t)AINPUT_SOURCE_TOUCHSCREEN;
    if (has(m,"supportsSource"))      { int src = va_arg(va,int); return pad ? ((src & NX_PAD_SOURCES) == src) : (src == AINPUT_SOURCE_TOUCHSCREEN); }
    if (has(m,"getVendorId"))         return pad ? 0x057e : 0;
    if (has(m,"getProductId"))        return pad ? 0x2009 : 0;
    if (has(m,"getControllerNumber")) return pad ? 1 : 0;
    if (has(m,"getKeyboardType"))     return 0;       /* KEYBOARD_TYPE_NONE */
    if (has(m,"isVirtual"))           return 0;
    if (has(m,"isEnabled"))           return 1;
    if (has(m,"isExternal"))          return pad ? 1 : 0;
    if (has(m,"hasMicrophone"))       return 0;
    if (has(m,"getKeyCharacterMap"))  return 0;
    return 0;
  }
  if (recv && ((const uint32_t *)recv)[0] == UMR_TAG) {
    const UMotionRange *r = recv;
    if (has(m,"getAxis"))   return (uint64_t)r->axis;
    if (has(m,"getSource")) return (uint64_t)r->source;
    return 0;
  }
  if (recv && ((const uint32_t *)recv)[0] == ULS_TAG) {
    const UList *l = recv;
    if (has(m,"size"))    return (uint64_t)l->n;
    if (has(m,"isEmpty")) return l->n == 0;
    return 0;
  }
  if (!e || e->tag!=UI_TAG) return 0;

  /* shared InputEvent base */
  if (has(m,"getDeviceId")) return (uint64_t)e->device_id;
  if (has(m,"getSource"))   return (uint64_t)(e->source ? e->source : (e->kind==KIND_MOTION?AINPUT_SOURCE_TOUCHSCREEN:AINPUT_SOURCE_KEYBOARD));
  if (has(m,"isFromSource")) { int src = va_arg(va,int); int have = e->source ? e->source : AINPUT_SOURCE_TOUCHSCREEN; return (have & src) == src; }
  if (has(m,"getEventTime")||has(m,"getDownTime")) return (uint64_t)e->time_ms; /* long */
  if (has(m,"getMetaState")) return 0;
  if (has(m,"getFlags"))     return 0;

  if (e->kind==KIND_MOTION){
    if (has(m,"getActionMasked")) return (uint64_t)(e->action & AMOTION_ACTION_MASK);
    if (has(m,"getActionIndex"))  return (uint64_t)((e->action>>AMOTION_ACTION_PTR_IDX_SHIFT)&0xff);
    if (has(m,"getAction"))       return (uint64_t)e->action;
    if (has(m,"getPointerCount")) return (uint64_t)e->count;
    if (has(m,"getPointerId")){ int i=va_arg(va,int); return (uint64_t)((i>=0&&i<e->count)?e->ids[i]:0); }
    if (has(m,"getToolType"))     return AMOTION_TOOL_TYPE_FINGER;
    if (has(m,"getButtonState"))  return 0;
    if (has(m,"getHistorySize"))  return 0;     /* no batched history -> engine skips getHistorical* */
    return 0;
  }
  /* KeyEvent */
  if (has(m,"getKeyCode"))     return (uint64_t)e->keycode;
  if (has(m,"getAction"))      return (uint64_t)e->action;
  if (has(m,"getRepeatCount")) return 0;
  if (has(m,"getUnicodeChar")||has(m,"GetUnicodeChar")) return 0;
  return 0;
}

/* ---- float getters ------------------------------------------------------ */
float input_dispatch_float(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  ILOG("    [in.f] %s  (cls=%s)\n", m, id->cls);
  if (recv && ((const uint32_t *)recv)[0] == UMR_TAG) {
    const UMotionRange *r = recv;
    if (has(m,"getMin"))   return r->lo;
    if (has(m,"getMax"))   return r->hi;
    if (has(m,"getFlat"))  return r->flat;
    if (has(m,"getFuzz"))  return r->fuzz;
    if (has(m,"getRange")) return r->hi - r->lo;
    if (has(m,"getResolution")) return 0.0f;
    return 0.0f;
  }
  if (!e || e->tag!=UI_TAG || e->kind!=KIND_MOTION) return 0.0f;
  /* getAxisValue(axis[, pointerIndex]) -- joystick events */
  if (has(m,"getAxisValue")) { int axis = va_arg(va,int); return (axis >= 0 && axis < UI_MAX_AXES) ? e->axes[axis] : 0.0f; }
  int i = ptr_index(id, va);
  if (i<0 || i>=e->count) i=0;
  if (has(m,"getRawX")||(has(m,"getX"))) return e->count? e->xs[i] : 0.0f;
  if (has(m,"getRawY")||(has(m,"getY"))) return e->count? e->ys[i] : 0.0f;
  if (has(m,"getPressure"))   return 1.0f;
  if (has(m,"getSize"))       return 0.1f;
  if (has(m,"getOrientation"))return 0.0f;
  return 0.0f;
}

/* ---- object-returning InputDevice / InputManager surface ------------------
 * Called from jni_fake dispatch_object when input_owns_class() or
 * input_owns_recv() says so. Static InputDevice.getDevice(id) arrives with the
 * class as receiver, so it is matched by name alone. */
void *input_dispatch_object(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *m = id->name;
  pad_ranges_init();
  /* static InputDevice.getDeviceIds() -> int[] {0, 1} */
  if (has(m,"getDeviceIds")) {
    static const int ids[2] = { 0, 1 };
    return jni_make_int_array(ids, 2);
  }
  /* static InputDevice.getDevice(int id) */
  if (has(m,"getDevice") && !has(m,"getDeviceIds")) {
    int want = va_arg(va, int);
    return (want == 0 || want == 1) ? unity_inputdevice(want) : NULL;
  }
  if (input_is_device(recv)) {
    const UDevice *d = recv; const int pad = d->id == 1;
    if (has(m,"getName"))       return jni_make_string(pad ? "Nintendo Switch Pro Controller" : "touchscreen");
    if (has(m,"getDescriptor")) return jni_make_string(pad ? "nx-pro-controller-057e-2009" : "nx-touchscreen");
    if (has(m,"getMotionRanges")) return pad ? (void *)&g_pad_range_list : (void *)&g_pad_range_list; /* touch: same list is harmless */
    if (has(m,"getMotionRange")) {
      int axis = va_arg(va, int);
      for (int i = 0; i < g_pad_range_list.n; i++)
        if (((UMotionRange *)g_pad_range_list.items[i])->axis == axis) return g_pad_range_list.items[i];
      return NULL;
    }
    if (has(m,"hasKeys")) {                     /* (int[] keys) -> boolean[] all true */
      void *arr = va_arg(va, void *);
      int len = arr ? jni_array_length(arr) : 0;
      return jni_make_bool_array_filled(len, 1);
    }
    if (has(m,"getKeyCharacterMap")) return NULL;
    return NULL;
  }
  if (recv && ((const uint32_t *)recv)[0] == ULS_TAG) {
    const UList *l = recv;
    if (has(m,"get")) { int i = va_arg(va, int); return (i >= 0 && i < l->n) ? l->items[i] : NULL; }
    if (has(m,"iterator") || has(m,"toArray")) return NULL;   /* Rewired indexes with size()/get(i) */
    return NULL;
  }
  return NULL;
}
