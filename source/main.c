/* main.c -- ZOOKEEPER DX Switch wrapper entry point.
 *
 * Unity 2022.3 / IL2CPP. Loads libmain + libunity + libil2cpp, then drives the
 * lifecycle the Java UnityPlayer normally runs (initJni -> recreate GFX state ->
 * render loop), calling the native entry points recovered from libunity.so's
 * JNI_OnLoad (see unity_entrypoints.h). The engine owns its own EGL/GLES3 context
 * created from android_native_window(); SDL is audio/HID only.
 *
 * Heap + syscall scaffolding adapted from cr3_nx's main.c (MIT).
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "nx_patch_dfu.h"   /* Daggerfall: derived libunity patch + phase flags */
#include "dfu_input.h"
#include "dfu_keyboard.h"
#include "dfu_offsets.h"    /* Daggerfall: every libil2cpp address, derived */
#include "nx_data_root.h"   /* g_data_root / nx_path: runtime folder resolution */

/* Switch pad -> Daggerfall action bridge (dfu_input.c). Both calls are
 * no-ops until il2cpp is up and every entry point has passed its guard. */
void dfu_input_init(void);
void dfu_input_pump(void);
#include <dirent.h>
#include <strings.h>  /* strncasecmp */
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "diag.h"

/* DATA_ROOT is now the RUNTIME-resolved root (nx_data_root.c), not a
 * compile-time string. Sites that used string concatenation -- DATA_ROOT "/x"
 * -- cannot work against a variable and have been converted to nx_path("/x").
 * The compile-time default still exists as DATA_ROOT_DEFAULT in
 * nx_data_root.c, used only as the last-resort fallback. */
#define DATA_ROOT  g_data_root
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"
#define LIB_FB_APP          "libFirebaseCppApp-11_9_0.so"
#define LIB_FB_ANALYTICS    "libFirebaseCppAnalytics.so"
#define LIB_FB_MESSAGING    "libFirebaseCppMessaging.so"
#define LIB_FB_REMOTECONFIG "libFirebaseCppRemoteConfig.so"

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena. In overcommit mode (g_overcommit) this is a big *virtual* window in
 * the alias region: Unity's PROT_NONE pool reservations cost only address space
 * and physical pages are committed on demand via svcMapPhysicalMemory. In the
 * fallback path it's a fully heap-backed 256MB-aligned slab (the Switch has no
 * native overcommit). Consumed by mmap_fake/munmap_fake (libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;          /* 1 = alias-region on-demand commit */
u64    g_alias_base = 0, g_alias_size = 0;
/* captured in __libnx_initheap for logging from main() (log file isn't open yet) */
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
/* granular setup diagnostics so a failed gate tells us WHICH step bailed */
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
/* stack-region overcommit arena armer (libc_shim.c) */
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;   /* system resource size (0 => svcMapPhysicalMemory unusable) */

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Replacement icall for UnityEngine.Application::get_internetReachability.
 * Returns NetworkReachability.NotReachable (0). See the frame-0 install site
 * for why (unblocks FirebaseManager.IsGetMessage / the boot coroutine). The
 * il2cpp icall ABI for this static getter is "int32_t func(MethodInfo*)"; we
 * ignore the hidden arg and just report no network. */
static int32_t nx_internet_reachability(void) { return 0; }

/* Replacement for Common.FirebaseManager.IsGetMessage (instance method, returns
 * bool). The FirebaseLoading boot state spins on this; force it complete and log
 * once so the run tells us whether state 5 is even reached. */
static int nx_rd(uintptr_t a);            /* fwd decl: readable-memory check  */
static uintptr_t nx_pd(uintptr_t a);      /* fwd decl: safe pointer deref      */
static int32_t nx_is_get_message(void) {
  static int once = 0;
  if (!once) { once = 1; debugPrintf("[hook] FirebaseManager.IsGetMessage reached -> forced 1\n"); }
  return 1;
}

/* Unity's native time base is frozen in our environment (it never advances the
 * engine clock -- likely it expects Android Choreographer frame timestamps we
 * don't deliver). Every managed Time.* accessor therefore reads a frozen value:
 * deltaTime==0, time/realtimeSinceStartup constant. That freezes all time-based
 * game logic -- DOTween (the boot logo fade), WaitForSeconds, etc. -- which is
 * what holds the black screen (the fade never completes, so boot never starts).
 * Work around it by driving our own monotonic frame clock and redirecting the
 * managed Time accessors to it. nx_time_tick() runs once per render-loop frame. */
static volatile float  g_unity_dt   = 1.0f / 60.0f;   /* SCALED   */
static volatile double g_unity_time = 0.0;            /* SCALED   */
static volatile float  g_unity_unscaled_dt   = 1.0f / 60.0f;
static volatile float  g_unity_smooth_dt     = 1.0f / 60.0f;
static volatile double g_unity_unscaled_time = 0.0;
static volatile uint32_t g_frame_count = 0;   /* Time.frameCount source */
static uint64_t g_time_prev_ns  = 0;
static uint64_t g_time_start_ns = 0;
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* Live Time.timeScale, read from the engine each frame.
 *
 * We deliberately do NOT hook get_timeScale / set_timeScale. The game owns that
 * value -- GameManager::PauseGame writes it, and Daggerfall pauses on every
 * inventory screen, menu and rest -- so the wrapper reads it rather than trying
 * to mirror it. One source of truth, and no setter to trampoline.
 *
 * get_timeScale is an il2cpp icall resolver stub (resolves
 * "UnityEngine.Time::get_timeScale()", caches the pointer, tail-calls it), so
 * calling it returns the true engine value. Static, no MethodInfo argument.
 * Same threading rule as the input bridge: render thread only. */
/* Pointer to the ICALL CACHE SLOT, not to the stub. See dfu_offsets.h. */
static float (* volatile *g_timescale_slot)(void);
static float g_time_scale = 1.0f;

static void note_managed_live(void);

/* UnityEngine.Input hooks. Static methods, so x0 is the KeyCode; we return a
 * bool in w0. Marking managed-live here too: only managed code can reach these. */
static uint8_t hk_Input_GetKey(int kc)     { note_managed_live(); return (uint8_t)dfu_kc_is_down(kc); }
static uint8_t hk_Input_GetKeyDown(int kc) { note_managed_live(); return (uint8_t)dfu_kc_went_down(kc); }
static uint8_t hk_Input_GetKeyUp(int kc)   { note_managed_live(); return (uint8_t)dfu_kc_went_up(kc); }

static void nx_time_tick(void) {
  uint64_t now = nx_now_ns();
  g_frame_count++;                    /* advance Time.frameCount once per frame */
  if (!g_time_start_ns) g_time_start_ns = now;

  /* Read the slot the engine fills in when IT resolves get_timeScale. Calling
   * the resolver stub ourselves is what crashed the first boot: on frame 1 the
   * icall table is empty, the resolve fails, and the runtime faults building
   * the MissingMethodException it wanted to throw. Piggy-backing on the game's
   * own resolution means we only ever call a pointer IL2CPP produced. */
  float (*ts_fn)(void) = g_timescale_slot ? *g_timescale_slot : NULL;
  if (ts_fn) {
    static int announced = 0;
    if (!announced) { announced = 1;
      debugPrintf("[time] timeScale icall now resolved by the game -- "
                  "honouring Time.timeScale from frame %u\n", g_frame_count); }
    float ts = ts_fn();
    if (!(ts >= 0.0f)) ts = 0.0f;      /* clamp negatives; also catches NaN */
    if (ts > 100.0f)   ts = 100.0f;
    g_time_scale = ts;
  }

  if (g_time_prev_ns) {
    double dt = (double)(now - g_time_prev_ns) / 1e9;
    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;            /* clamp, mirrors Unity maximumDeltaTime */
    g_unity_unscaled_dt = (float)dt;
    g_unity_unscaled_time += dt;
    double sdt = dt * (double)g_time_scale;
    g_unity_dt = (float)sdt;
    g_unity_time += sdt;
    /* smoothDeltaTime: Unity low-passes it. Exponential average over ~10
     * frames, on the SCALED delta so a pause flattens it too. */
    g_unity_smooth_dt += ((float)sdt - g_unity_smooth_dt) * 0.1f;
  }
  g_time_prev_ns = now;
}
static float nx_delta_time(void)          { note_managed_live(); return g_unity_dt; }
static float nx_unscaled_delta_time(void) { note_managed_live(); return g_unity_unscaled_dt; }
static float nx_smooth_delta_time(void)   { note_managed_live(); return g_unity_smooth_dt; }
static float nx_time_f(void)              { note_managed_live(); return (float)g_unity_time; }
static float nx_unscaled_time_f(void)     { note_managed_live(); return (float)g_unity_unscaled_time; }
static int   nx_frame_count(void){ note_managed_live(); return (int)g_frame_count; }
uint32_t     port_frame_count(void){ return g_frame_count; } /* audio pump warmup gate */

/* "Managed code is executing." Set by the Time.get_* hooks, which are only ever
 * reached from managed code -- so the first call proves IL2CPP is initialised,
 * its icall table is populated, and this thread can safely enter managed land.
 *
 * This exists because calling into IL2CPP before that point is not a subtle
 * error, it is an immediate crash, and it happened: nx_time_tick() called the
 * get_timeScale resolver stub on frame 1 and the runtime faulted building the
 * MissingMethodException it wanted to throw (AUDIT.md sec 15). The input bridge
 * had the same hazard and is now gated on this too. */
static volatile int g_managed_live = 0;
int  port_managed_live(void) { return g_managed_live; }
static void note_managed_live(void) {
  if (!g_managed_live) {
    g_managed_live = 1;
    debugPrintf("[boot] managed code is live (frame %u) -- IL2CPP entry now "
                "permitted for input\n", g_frame_count);
  }
}
static float nx_realtime_since_startup(void) {
  uint64_t now = nx_now_ns();
  if (!g_time_start_ns) g_time_start_ns = now;
  return (float)((double)(now - g_time_start_ns) / 1e9);
}

/* fbstub42: TimeManager::Update entry hook. The Switch port's player loop drives
 * Update with a frozen vsync timestamp as newTime, so deltaTime collapses to the
 * 1e-5 floor and every native time reader (the PreloadManager included) starves,
 * which is what wedges async scene loading (the resident-scene black screen). We
 * redirect Update's entry (libunity 0x446114) here, replay its tiny prologue
 * (frameCount++, aux counter++, pause check), then re-enter its body (0x446138 --
 * frameless, re-reads everything from x0) with newTime = GetTimeSinceStartup(),
 * the engine's own monotonic clock (0x446578) which DOES advance. Update then
 * derives all deltaTime variants and m_Time correctly. Offsets verified against
 * both the game binary and the symbolized 2022.3.62f2 reference engine. */
static void   (*g_unity_update_body)(void *, double) = NULL; /* 0x4f04a4 Update body */
static void     *g_tm = NULL;                       /* captured TimeManager instance */
static void   *(*g_get_time_manager)(void) = NULL;  /* 0x4f0a7c GetTimeManager() (subsystem 7) */
static uint64_t  g_clk_base_ns = 0;
static volatile uint64_t g_last_main_tick_ns = 0;
static Mutex     g_clock_lock;                       /* main-hook vs clock-thread */
static Thread    g_clock_thr;
#define CLOCK_STALL_NS 100000000ULL                  /* 100ms main silence => stalled */
/* Re-run Update's body with a wall-clock newTime so deltaTime/m_Time advance even
 * while UnityMain is parked in a synchronous scene-load (the frame-0 async hang). */
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall    = (double)(now - g_clk_base_ns) / 1e9;
  double sref    = *(volatile double *)((char *)tm + 0xe8);   /* m_StartupRef */
  double newTime = sref + wall;
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;
  { static int once = 0; if (!once) { once = 1; debugPrintf("[time] Update hook first fire (tm=%p)\n", tm); } }
  g_last_main_tick_ns = nx_now_ns();                          /* main thread is live */
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;            /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;            /* aux counter++           */
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return; /* paused -> early return  */
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);                              /* ~8ms keep-alive */
    /* Fetch the TimeManager singleton directly -- the entry hook at 0x4f0480 never
     * fires during PvZ's boot-coroutine save load (the player loop isn't ticking
     * Update yet), so g_tm stays NULL. GetTimeManager() returns it once the
     * subsystem exists; until then it's NULL and we skip. */
    void *tm = g_get_time_manager ? g_get_time_manager() : g_tm;
    { static void *seen = NULL; if (tm && tm != seen) { seen = tm; debugPrintf("[time] clock thread got TimeManager=%p (driving native clock)\n", tm); } }
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {                       /* only while main is silent */
      nx_clock_tick(tm);
      { static unsigned _n = 0; if ((_n++ & 0x3f) == 0) debugPrintf("[time] clock thread driving Update (main stalled)\n"); }
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  /* Symbol-derived for THIS build -- see dfu_offsets.h "libunity, from symbols". */
  g_unity_update_body = (void (*)(void *, double))(ub + OFF_TimeManager_Update_body);
  g_get_time_manager  = (void *(*)(void))(ub + OFF_GetTimeManager);

  /* Guard: TimeManager::Update opens `ldr x8,[x0,#0xc8]` (m_FrameCount). This is
   * a blind .text write, so refuse rather than corrupt if the binary moved. */
  uint32_t have = *(volatile uint32_t *)(ub + OFF_TimeManager_Update);
  if (have != DFU_TIMEMGR_GUARD_WORD) {
    debugPrintf("[boot] !! TimeManager::Update @libunity+0x%x guard MISMATCH "
                "(have 0x%08x, want 0x%08x) -- time fix NOT installed\n",
                OFF_TimeManager_Update, have, DFU_TIMEMGR_GUARD_WORD);
    return;
  }
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  so_patch_code((void *)(ub + OFF_TimeManager_Update), stub, sizeof stub);
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
  debugPrintf("[boot] installed TimeManager::Update hook @libunity+0x%x "
              "+ clock thread (newTime <- startupRef + wallclock)\n", OFF_TimeManager_Update);
}

/* --- boot finish-flag probe -------------------------------------------------
 * State 6 (SetFinishFlag) walks A=*(il2cpp+0x21fd0718); B=*A; obj=*(B+0x20);
 * obj2=*(*(obj+0xc0)+0x10); holder=*(obj2+0xb8 then deref); and writes
 * *(holder+0x10)=1 -- but ONLY if holder is non-null (else it stays at state 6
 * forever). CheckInitializeFinish polls the same obj2 (+0xe0 / +0x135). We read
 * the live chain so we can see exactly which link is null / whether the finish
 * flag ever gets set, without any destructive patching. */
static int nx_rd(uintptr_t a) {
  MemoryInfo mi; u32 pi;
  if (a < 0x1000) return 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
  if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) return 0;
  return 1;
}
static uintptr_t nx_pd(uintptr_t a) { return nx_rd(a) ? *(volatile uintptr_t *)a : 0; }
__attribute__((unused)) static void nx_probe_finish(uintptr_t base) {
  uintptr_t A    = nx_pd(base + 0x21fd0718);
  uintptr_t B    = A    ? nx_pd(A)          : 0;
  uintptr_t obj  = B    ? nx_pd(B + 0x20)   : 0;
  uintptr_t c0   = obj  ? nx_pd(obj + 0xc0) : 0;
  uintptr_t obj2 = c0   ? nx_pd(c0 + 0x10)  : 0;
  uintptr_t b8   = obj2 ? nx_pd(obj2 + 0xb8): 0;
  uintptr_t hold = b8   ? nx_pd(b8)         : 0;
  int      flag  = (hold && nx_rd(hold + 0x10)) ? *(volatile uint8_t  *)(hold + 0x10) : -1;
  uint32_t e0    = (obj2 && nx_rd(obj2 + 0xe0)) ? *(volatile uint32_t *)(obj2 + 0xe0) : 0xffffffffu;
  int o135  = (obj  && nx_rd(obj + 0x135))  ? *(volatile uint8_t *)(obj + 0x135)  : -1;
  int o2135 = (obj2 && nx_rd(obj2 + 0x135)) ? *(volatile uint8_t *)(obj2 + 0x135) : -1;
  debugPrintf("[probe] A=%p B=%p obj=%p obj2=%p hold=%p | finishFlag=%d e0=0x%x obj.135=%d obj2.135=%d\n",
              (void *)A, (void *)B, (void *)obj, (void *)obj2, (void *)hold, flag, e0, o135, o2135);
}


/* libunity ~17M + libil2cpp ~36M + headroom for relocated segments */
#define SO_REGION_BYTES (160u * 1024 * 1024)

/* Reserve the virtual arena window at the TOP of the alias region (deep in the
 * 64GB region, where libnx never allocates) after verifying it is fully unmapped.
 * No physical backing yet -- pages are committed on demand. */
static void *overcommit_reserve_window(size_t size) {
  size = (size + MMAP_ARENA_ALIGN - 1) & ~(MMAP_ARENA_ALIGN - 1);
  if (!g_alias_base || g_alias_size < size + MMAP_ARENA_ALIGN) return NULL;
  u64 top = g_alias_base + g_alias_size;
  u64 win = (top - size) & ~(MMAP_ARENA_ALIGN - 1);
  u64 a = win;
  while (a < win + size) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return NULL;
    if (mi.type != MemType_Unmapped) return NULL;   /* collision -> bail */
    a = mi.addr + mi.size;
  }
  return (void *)win;
}

/* virtmemFindStack refuses large windows even when the stack region has room, so
 * scan the region directly via svcQueryMemory for the largest 256MB-aligned
 * unmapped hole (and log the whole map for diagnosis). svcMapMemory only aliases
 * into the stack region, so the OC window must live here. */
static void  *g_oc_win2    = NULL;   /* second-largest stack hole (OC window 2) */
static size_t g_oc_win2_sz = 0;
extern int oc_arena_add_window(void *window, size_t window_bytes);
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0, sec_a = 0, sec_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;                              /* no-progress guard */
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { sec_l = best_l; sec_a = best_a; best_l = he - hs; best_a = hs; }
        else if (he - hs > sec_l) { sec_l = he - hs; sec_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  if (sec_a) {   /* stash the runner-up for OC window 2 */
    u64 a2 = (sec_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
    if (a2 < sec_a + sec_l) {
      u64 v2 = ((sec_a + sec_l) - a2) & ~(MMAP_ARENA_ALIGN - 1);
      if (v2 > want) v2 = want;
      if (v2) { g_oc_win2 = (void *)a2; g_oc_win2_sz = v2; }
    }
  }
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

/* Try to set up alias-region overcommit, recording each step's outcome into the
 * g_oc_* globals (logged from main). Alias-region overcommit turned out to be
 * impossible on this process: svcMapPhysicalMemory requires a non-zero kernel
 * "system resource" pool (for page-table/block bookkeeping) and our title-override
 * process has none -> it returns InvalidState (0xfa01). The unsafe pool
 * (svcMapPhysicalMemoryUnsafe) is ~44MB and already consumed. So we just record the
 * diagnostics and stay on the fully heap-backed arena. (Confirmed via Atmosphere
 * kern_svc_physical_memory.cpp: `R_UNLESS(GetTotalSystemResourceSize() > 0,
 * ResultInvalidState())`.) */
static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap the engine mallocs from. (Verbatim from cr3_nx.) */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  /* Preferred path: alias-region overcommit. Secures the virtual window and
   * test-commits a page FIRST, then shrinks the heap to [newlib + so_zone] so the
   * freed physical (~2.5GB) is available for on-demand commits. Everything is
   * secured before the shrink so a failure can't strand us with a shrunk heap. */
  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed 256MB-aligned arena (no overcommit). */
  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);   /* clamp to RAM */
    fake_heap_size = size - so_zone - arena_sz - big_align;       /* newlib gets the rest */
  } else {
    /* heap too small for a dedicated arena (e.g. applet mode): skip it; the mmap
     * allocator falls back to a memalign-backed bitmap arena. */
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* Unity ships Android player data in one of TWO layouts, and which one you get
 * is a build setting, not a property of the engine version:
 *
 *   MONOLITHIC   assets/bin/Data/data.unity3d          -- one archive
 *   LOOSE/SPLIT  assets/bin/Data/globalgamemanagers    -- plus level0, level1,
 *                sharedassetsN.assets.splitN, globalgamemanagers.assets.splitN,
 *                "unity default resources", unity_app_guid, and a pile of
 *                hash-named resource files
 *
 * The PvZ base this forked from is a MONOLITHIC build, so `data.unity3d` was
 * hardcoded here as a required file. Daggerfall's APK is a LOOSE build and has
 * no data.unity3d at all -- so this check failed and reported a missing file
 * that is not supposed to exist, while every byte of game data sat right there.
 *
 * Detect the layout instead of assuming one. */
static int g_loose_layout = -1;    /* -1 unknown, 0 monolithic, 1 loose */

static int have(const char *rel) {
  char p[768]; struct stat st;
  snprintf(p, sizeof p, "%s/%s", DATA_ROOT, rel);
  return stat(p, &st) == 0;
}

/* A loose build is many files, and a partial copy is a real failure mode --
 * card pulled early, copy tool skipping, FAT32 refusing something. Unity fails
 * obscurely on a gap in a split series, so count the run and say so plainly. */
static int count_splits(const char *stem) {
  char rel[256];
  int n = 0;
  for (;; n++) {
    snprintf(rel, sizeof rel, "assets/bin/Data/%s.split%d", stem, n);
    if (!have(rel)) break;
  }
  return n;
}

static void check_data(void) {
  const char *libs[] = { LIB_MAIN, LIB_UNITY, LIB_IL2CPP };
  char path[768];
  struct stat st;

  for (unsigned i = 0; i < sizeof(libs)/sizeof(*libs); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, libs[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n%s\nCheck your SD card layout.", libs[i]);
  }

  int mono  = have("assets/bin/Data/data.unity3d");
  int loose = have("assets/bin/Data/globalgamemanagers");
  g_loose_layout = loose && !mono;

  if (!mono && !loose) {
    /* Say what IS there. An error naming a file that never existed in this
     * build sends people hunting for a file they cannot produce. */
    char dir[768];
    snprintf(dir, sizeof dir, "%s/assets/bin/Data", DATA_ROOT);
    debugPrintf("[boot] !! no player data in %s\n", dir);
    DIR *d = opendir(dir);
    if (!d) {
      debugPrintf("[boot] !! that directory does not exist or cannot be opened\n");
      fatal_error("assets/bin/Data is missing.\n"
                  "Copy the APK's assets/ folder next to the .nro.");
    }
    struct dirent *de; int shown = 0;
    while ((de = readdir(d)) && shown < 12)
      if (de->d_name[0] != '.') { debugPrintf("[boot]    found: %s\n", de->d_name); shown++; }
    closedir(d);
    fatal_error("assets/bin/Data has neither data.unity3d nor\n"
                "globalgamemanagers, so it is not Unity player data.\n"
                "See debug.log for what was found there.");
  }

  /* IL2CPP metadata is required in BOTH layouts and is the other thing that
   * goes missing, because it sits three directories deep. */
  if (!have("assets/bin/Data/Managed/Metadata/global-metadata.dat"))
    fatal_error("Missing global-metadata.dat\n"
                "(assets/bin/Data/Managed/Metadata/).\n"
                "The assets/ folder was copied incompletely.");

  if (g_loose_layout) {
    int ggm = count_splits("globalgamemanagers.assets");
    int sa0 = count_splits("sharedassets0.assets");
    int sa1 = count_splits("sharedassets1.assets");
    debugPrintf("[boot] player data: LOOSE/SPLIT layout\n");
    debugPrintf("[boot]   globalgamemanagers.assets splits: %d\n", ggm);
    debugPrintf("[boot]   sharedassets0/1 splits: %d / %d\n", sa0, sa1);
    /* A loose build is hundreds of files (this one: 322, of which 293 are
     * hash-named resource files). Copying "assets" from a phone or an archive
     * tool that gives up partway is the likely way this goes wrong, and Unity
     * reports it as a corrupt asset rather than a missing one. Log the count so
     * the log answers the question instead of the player guessing. */
    {
      char dd[768]; snprintf(dd, sizeof dd, "%s/assets/bin/Data", DATA_ROOT);
      DIR *d = opendir(dd);
      int n = 0;
      if (d) { struct dirent *de;
               while ((de = readdir(d))) if (de->d_name[0] != '.') n++;
               closedir(d); }
      debugPrintf("[boot]   entries in assets/bin/Data: %d\n", n);
      if (n && n < 20)
        debugPrintf("[boot]   !! that is very few for a loose build -- the "
                    "assets/ copy is probably incomplete\n");
    }
    debugPrintf("[boot]   level0=%d level1=%d default-resources=%d guid=%d\n",
                have("assets/bin/Data/level0"), have("assets/bin/Data/level1"),
                have("assets/bin/Data/unity default resources"),
                have("assets/bin/Data/unity_app_guid"));
    if (!ggm)
      fatal_error("globalgamemanagers is present but its .split files\n"
                  "are not. The assets/ copy is incomplete.");
    if (!have("assets/bin/Data/level0"))
      fatal_error("level0 is missing from assets/bin/Data.\n"
                  "The assets/ copy is incomplete.");
  } else {
    debugPrintf("[boot] player data: MONOLITHIC (data.unity3d)\n");
  }
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  debugPrintf("[mod] %-14s virtbase=%p size=0x%zx  (resolve: addr - virtbase = vaddr)\n",
              name, mod->load_virtbase, mod->load_size);

  /* Is this the build every offset in this port was derived from?
   *
   * Nothing else checks. Most hooks guard themselves and skip politely on a
   * mismatch, but the 21 allocator patch sites in libunity are written
   * unconditionally -- on the wrong build they corrupt 21 words of whatever is
   * there and the process dies early somewhere unrelated. Reported crashes
   * "very early, in places I never saw" are exactly that shape, so say so
   * plainly instead of letting it happen. */
  {
    size_t want = 0;
    if      (!strcmp(name, LIB_UNITY))  want = EXPECT_LIBUNITY_BYTES;
    else if (!strcmp(name, LIB_IL2CPP)) want = EXPECT_LIBIL2CPP_BYTES;
    if (want && mod->so_size != want) {
      debugPrintf("[mod] !! %s is %zu bytes, expected %zu\n",
                  name, (size_t)mod->so_size, want);
      fatal_error("%s is not the build this port was made for.\n\n"
                  "It is %zu bytes; this port needs %zu.\n\n"
                  "Use the .so files from\n%s\n\n"
                  "Every patch address comes from that exact APK.",
                  name, (size_t)mod->so_size, want, EXPECT_APK_NAME);
    }
  }
  crx_resolve_imports(mod);   /* so_resolve(mod, dynlib_functions, ...) */
  /* NOTE: so_patch_stack_canaries() intentionally NOT called. Per-thread bionic
   * TLS (install_bionic_tls) makes the engine's stack-protector guard consistent,
   * so the canary checks pass on their own. NOPing 2000+ b.ne sites risked a
   * false-positive in non-canary code (e.g. allocator list logic) -> corruption. */
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* ---------------------------------------------------------------------------
 * In-memory libunity patch (ported from VLN's nx_patch_unity_regions): the SD
 * card now ships the STOCK libunity.so and the boot patches it after load,
 * instead of distributing a pre-modified binary. 23 instruction words, from a
 * byte-exact diff of the known-good patched .so vs stock (Unity 2022.3.62f2):
 * 21 sites relax the allocator's memory-region granularity 256MB->64MB so the
 * engine fits the so_loader address space on a 4GB Switch, plus two branch
 * forces (0x5d24cc cond->uncond, 0x5d4e98 ldr->skip) from the same known-good
 * build. Verify-first like VLN: every original word must match before anything
 * is written; a fully pre-patched .so is detected and accepted; any other
 * mismatch leaves the binary untouched (different Unity build) with a loud log.
 * ------------------------------------------------------------------------- */
static int nx_patch_libunity(uintptr_t ub) {
  /* Daggerfall Unity (Unity 2022.3.62f3): tables live in nx_patch_dfu.h.
   * VERIFY-FIRST like the original -- every {from} word must match before ANY
   * write; a fully pre-patched .so is accepted; any mismatch patches nothing. */
  const NxPatchWord *P = DFU_PATCH_WORDS;
  const int N = DFU_PATCH_WORDS_N;
  int stock = 0, patched = 0;
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur == P[i].from) stock++;
    else if (cur == P[i].to) patched++;
    else {
      debugPrintf("[patch] libunity word mismatch @+0x%x: have 0x%08x want 0x%08x -> SKIP all (offset table may be off for this build)\n",
                  (unsigned)P[i].off, cur, P[i].from);
      return 0;
    }
  }
  if (patched == N) {
    debugPrintf("[patch] libunity already pre-patched (%d sites) -- ok\n", N);
  } else if (stock != N) {
    debugPrintf("[patch] libunity PARTIALLY patched (%d/%d) -> SKIP (won't mix builds)\n", patched, N);
    return 0;
  } else {
    for (int i = 0; i < N; i++)
      so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to);
    debugPrintf("[patch] libunity region granularity 256MB->64MB patched (%d sites)\n", N);
  }
  /* Optional branch forces (see nx_patch_dfu.h). */
  if (DFU_BRANCH_FORCES_N > 0) {
    const NxPatchWord *B = DFU_BRANCH_FORCES;
    for (int i = 0; i < DFU_BRANCH_FORCES_N; i++) {
      uint32_t cur = *(volatile uint32_t *)(ub + B[i].off);
      if (cur == B[i].from)
        so_patch_code((void *)(ub + B[i].off), &B[i].to, sizeof B[i].to);
      else if (cur != B[i].to)
        debugPrintf("[patch] branch-force @+0x%x mismatch (have 0x%08x) -> skip\n",
                    (unsigned)B[i].off, cur);
    }
    debugPrintf("[patch] libunity branch forces applied (%d sites)\n", DFU_BRANCH_FORCES_N);
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * PvZ 62f1c1 first-boot il2cpp hacks. With the GC stop-the-world bridge fixed for
 * PvZ (libc_shim.c pthread_kill_gc now reads the correct suspend/restart/ack
 * globals), the Boehm GC works, so -- exactly like the VLN port -- we do NOT try to
 * disable it. An earlier attempt to il2cpp_gc_disable() mid-il2cpp_init deadlocked
 * on a GC lock held by a not-yet-running helper thread (verified on hardware: hung
 * right after set_mode, UnityMain parked in a futex lock). The GC now runs normally
 * and the bridge keeps its POSIX-signal stop-the-world from hanging. All this does
 * is redirect Time.get_* to our frame clock. Called once at boot, before the loop.
 * ------------------------------------------------------------------------- */
static int g_boot_hacks_done = 0;
static void nx_boot_il2cpp_hacks(void) {
  if (g_boot_hacks_done) return;
  g_boot_hacks_done = 1;
  uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;

  /* Redirect managed Time.get_* to our frame clock. RVAs from dump.cs
   * (UnityEngine.Time, TypeDefIndex 8981), centralised in dfu_offsets.h.
   *
   * GUARDED, unlike the PvZ base. These are blind writes into .text: a wrong
   * address silently overwrites an instruction and the failure surfaces much
   * later as an unrelated crash. Every one of these seven getters begins with
   * 0xA9BF4FFE (`stp x30,x19,[sp,#-0x10]!`) in this binary -- verified -- so we
   * check that first word and refuse to hook anything that does not match.
   * A skipped hook logs loudly and costs you frame-clock accuracy; a wrong
   * hook costs you the boot with no clue why. */
  struct { uint32_t off; void *fn; const char *nm; } th[] = {
    { OFF_Time_realtimeSinceStartup, (void *)&nx_realtime_since_startup, "realtimeSinceStartup" },
    { OFF_Time_deltaTime,            (void *)&nx_delta_time,             "deltaTime" },
    { OFF_Time_unscaledDeltaTime,    (void *)&nx_unscaled_delta_time,    "unscaledDeltaTime" },
    { OFF_Time_smoothDeltaTime,      (void *)&nx_smooth_delta_time,      "smoothDeltaTime" },
    { OFF_Time_time,                 (void *)&nx_time_f,                 "time" },
    { OFF_Time_unscaledTime,         (void *)&nx_unscaled_time_f,        "unscaledTime" },
    { OFF_Time_renderedFrameCount,   (void *)&nx_frame_count,            "renderedFrameCount" },
    { OFF_Time_frameCount,           (void *)&nx_frame_count,            "frameCount" },
  };
  int hooked = 0, skipped = 0;
  for (unsigned i = 0; i < sizeof(th) / sizeof(th[0]); i++) {
    uint32_t have = *(volatile uint32_t *)(b + th[i].off);
    if (have != DFU_TIME_GUARD_WORD) {
      debugPrintf("[boot] !! Time.get_%s @il2cpp+0x%x guard MISMATCH "
                  "(have 0x%08x, want 0x%08x) -- NOT hooked\n",
                  th[i].nm, th[i].off, have, DFU_TIME_GUARD_WORD);
      skipped++;
      continue;
    }
    uint32_t s[4];
    s[0] = 0x58000050u; s[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
    memcpy(&s[2], &th[i].fn, 8);
    so_patch_code((void *)(b + th[i].off), s, sizeof s);
    debugPrintf("[boot] hooked Time.get_%s @il2cpp+0x%x\n", th[i].nm, th[i].off);
    hooked++;
  }
  {   /* get_timeScale: take the address of its ICALL CACHE SLOT. We never call
       * the stub -- doing that on frame 1 is what crashed the first boot. */
    uint32_t tw = *(volatile uint32_t *)(b + OFF_Time_timeScale);
    if (tw == DFU_TIME_GUARD_WORD) {
      g_timescale_slot = (float (* volatile *)(void))(b + OFF_Time_timeScale_ICACHE);
      debugPrintf("[boot] Time.timeScale: watching icall cache @il2cpp+0x%x "
                  "(stub @0x%x NEVER called by us)\n",
                  OFF_Time_timeScale_ICACHE, OFF_Time_timeScale);
    } else {
      debugPrintf("[boot] !! get_timeScale guard MISMATCH (0x%08x) -- timeScale "
                  "assumed 1.0; PAUSE WILL NOT STOP THE WORLD\n", tw);
    }
  }
  /* ---- UnityEngine.Input.GetKey / GetKeyDown / GetKeyUp -------------------
   * The pad answers through the game's OWN input path. See dfu_offsets.h for
   * why this direction (game calls us) is safe where the old SetKey bridge
   * (we call the game) was not. */
  {
    static const struct { uint32_t off; const char *nm; int kind; } ih[] = {
      { OFF_Input_GetKey,     "GetKey",     0 },
      { OFF_Input_GetKeyDown, "GetKeyDown", 1 },
      { OFF_Input_GetKeyUp,   "GetKeyUp",   2 },
    };
    for (unsigned i = 0; i < sizeof ih / sizeof *ih; i++) {
      uint32_t w = *(volatile uint32_t *)(b + ih[i].off);
      if (w != DFU_INPUT_GUARD_WORD) {
        debugPrintf("[boot] !! Input.%s @il2cpp+0x%x guard MISMATCH (0x%08x) "
                    "-- pad will not reach the game\n", ih[i].nm, ih[i].off, w);
        continue;
      }
      void *fn = ih[i].kind == 0 ? (void *)hk_Input_GetKey
               : ih[i].kind == 1 ? (void *)hk_Input_GetKeyDown
                                 : (void *)hk_Input_GetKeyUp;
      uint8_t s2[16];
      memcpy(s2 + 0, (const uint8_t[]){0x50,0x00,0x00,0x58}, 4);  /* ldr x16,#8 */
      memcpy(s2 + 4, (const uint8_t[]){0x00,0x02,0x1f,0xd6}, 4);  /* br  x16    */
      memcpy(s2 + 8, &fn, 8);
      so_patch_code((void *)(b + ih[i].off), s2, sizeof s2);
      debugPrintf("[boot] hooked Input.%s @il2cpp+0x%x\n", ih[i].nm, ih[i].off);
    }
  }

  dfu_kbd_init(b);  /* TextBox focus hook -> software keyboard */

  debugPrintf("[boot] Time hooks: %d installed, %d skipped\n", hooked, skipped);
  if (skipped) {
    debugPrintf("[boot] !! Your libil2cpp.so does not match the offsets this "
                "build was derived from. Run tools/verify_offsets.py.\n");
  }
  so_flush_caches(&il2cpp_mod);   /* make the Time-hook code patches live */
  debugPrintf("[boot] Time.get_* hooks installed; GC left running (handled by bridge)\n");
}

int main(int argc, char *argv[]) {
  /* MUST BE FIRST. debugPrintf() writes to g_log_path, which does not exist
   * until this runs -- so any logging before it goes nowhere, which is exactly
   * how a wrong root used to hide itself. */
  nx_resolve_data_root(argc, argv);

  socketInitializeDefault();
  debugPrintf("[boot] === daggerfall_nx start (Unity 2022.3.62f3) ===\n");
  debugPrintf("[boot] data root: %s\n", g_data_root);
  debugPrintf("[boot]        via: %s\n", g_data_root_how);
  if (!nx_have_arena2()) {
    debugPrintf("[boot] !! no arena2/ARENA2 folder under %s\n", g_data_root);
    debugPrintf("[boot] !! Daggerfall Unity needs the original Daggerfall game "
                "data. Put the ARENA2 folder next to the .so files.\n");
  } else {
    debugPrintf("[boot] arena2: present\n");
  }

  /* No config.txt. Every setting it used to carry is either fixed by what the
   * port can actually do (landscape only; English only, because the game's
   * Chinese path does not work here) or was never applied in the first place
   * (screen_width/height). See config.h for the reasoning on each. Nothing is
   * read from or written to the SD card at startup. */

  /* Sweep Unity's case-sensitivity probe files: CASESENSITIVETEST<guid> strays
   * from older builds, plus the single hidden scratch the probe is redirected
   * to now (libc_shim.c casetest_redirect). */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, "%s/%s", DATA_ROOT, de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  /* CWD fix (mirrors MMX enter_data_dir): title-override / hbloader leaves the
   * working dir at the .nro folder or the SD root, NOT the game dir. Unity &
   * il2cpp read many files through *relative* paths ("assets/bin/Data/...") and
   * our basename_fallback stats relative to cwd, so a wrong cwd silently yields
   * empty/missing reads -> NULL il2cpp classes. chdir into DATA_ROOT so every
   * relative read resolves under sdmc:/switch/zookeeper. (Absolute "sdmc:/..."
   * reads are unaffected.) */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    char cwd2[256] = {0};
    getcwd(cwd2, sizeof cwd2);
    struct stat st;
    /* Probe BOTH layout markers -- see check_data(). Reporting only
     * data.unity3d made a healthy loose build look broken. */
    int reach_mono  = stat("assets/bin/Data/data.unity3d", &st) == 0;
    int reach_loose = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    int reach_assets = reach_mono || reach_loose;
    int reach_meta   = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_guid   = stat("assets/bin/Data/unity_app_guid", &st) == 0;
    debugPrintf("[boot] cwd was '%s' -> chdir(%s)=%d -> '%s'\n", cwd, DATA_ROOT, rc, cwd2);
    debugPrintf("[boot] reachable(rel): player-data=%d (mono=%d loose=%d) "
                "metadata=%d unity_app_guid=%d\n",
                reach_assets, reach_mono, reach_loose, reach_meta, reach_guid);
  }

  /* Force libunity to RE-EXTRACT il2cpp resources every boot. Observed: when
   * extraction is skipped (il2cpp/unity.ver present), il2cpp mmaps the extracted
   * global-metadata.dat and crashes in Class::Init(NULL); when extraction RUNS,
   * il2cpp uses the full source it reads for the copy and gets past that point.
   * The extracted copy is bad because our shim doesn't flush a writable
   * file-backed mmap back to disk, so it lands truncated. Removing the extracted
   * markers makes libunity redo the extraction each boot (uses the good source).
   * Proper fix = flush writable file-backed mmaps on munmap (tracked separately). */
  {
    int a = unlink(nx_path("/il2cpp/unity.ver"));
    int b = unlink(nx_path("/il2cpp/Metadata/global-metadata.dat"));
    int c = unlink(nx_path("/il2cpp/Resources/mscorlib.dll-resources.dat"));
    debugPrintf("[boot] force re-extract: unlink unity.ver=%d metadata=%d resources=%d\n", a, b, c);
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem layout: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    if (g_overcommit)
      debugPrintf("[boot] OVERCOMMIT on: heap shrunk to %u MB, freed %u MB physical; "
                  "arena reserved virtual @ %p (commit on demand)\n",
                  g_oc_heap_mb, g_oc_freed_mb, g_mmap_arena_base);
    else
      debugPrintf("[boot] OVERCOMMIT off (heap-backed): system_resource=%u MB "
                  "(svcMapPhysicalMemory needs >0; unsafe pool exhausted). map_hint=%d alias=%u MB\n",
                  (unsigned)(g_oc_sysres >> 20), g_oc_hint_map, g_oc_alias_mb);
  }

  /* Overcommit feasibility probe. Proper PROT_NONE overcommit on Switch needs a
   * physical-backing primitive (svcMapMemory in the stack region, or
   * svcMapPhysicalMemory in the alias region) plus a region large enough to hold
   * Unity's multi-GB reservations. MMX found svcMapMemory caps at ~2-3 pools (the
   * stack region is ~1GB). Log the region sizes + which mapping svc are granted so
   * we can size/choose the real overcommit (or rule it out) from real numbers. */
  {
    struct { const char *nm; int a, s; } R[] = {
      { "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
      { "heap",  InfoType_HeapRegionAddress,  InfoType_HeapRegionSize  },
      { "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
    };
    for (unsigned i = 0; i < 3; i++) {
      u64 a = 0, s = 0;
      svcGetInfo(&a, R[i].a, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&s, R[i].s, CUR_PROCESS_HANDLE, 0);
      debugPrintf("[probe] region %-5s base=0x%lx size=%u MB\n",
                  R[i].nm, (unsigned long)a, (unsigned)(s >> 20));
    }
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[probe] mem total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20),
                (unsigned)((tot - used) >> 20));
    debugPrintf("[probe] svc hinted: MapPhysicalMemory(0x2c)=%d UnmapPhysical(0x2d)=%d "
                "MapMemory(0x24)=%d UnmapMemory(0x25)=%d\n",
                envIsSyscallHinted(0x2c), envIsSyscallHinted(0x2d),
                envIsSyscallHinted(0x24), envIsSyscallHinted(0x25));
  }

  /* Decisive probe: does svcMapMemory accept a dst in the (unmapped upper) HEAP
   * region? The stack region works but is only ~2GB (~8 regions). The heap region
   * is 8GB; its upper ~5GB sits unmapped above our heap. If svcMapMemory works
   * there too, we can host Unity's 256MB pools in ~7GB of backable address space
   * (~28 regions) without any libunity patching. Pure diagnostic: map 1 page,
   * verify the sentinel reads back, unmap. */
  {
    u64 hbase = 0, hsize = 0;
    svcGetInfo(&hbase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&hsize, InfoType_HeapRegionSize,    CUR_PROCESS_HANDLE, 0);
    u64 probe = 0, a = hbase, end = hbase + hsize;
    while (a < end) {
      MemoryInfo mi; u32 pi;
      if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
      if (mi.addr + mi.size <= a) break;
      if (mi.type == MemType_Unmapped && mi.size >= MMAP_ARENA_ALIGN) {
        u64 al = (mi.addr + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
        if (al + 0x1000 <= mi.addr + mi.size) { probe = al; break; }
      }
      a = mi.addr + mi.size;
    }
    if (probe) {
      void *src = memalign(0x1000, 0x1000);
      if (src) {
        *(volatile u32 *)src = 0xABCD1234;
        Result rc = svcMapMemory((void *)probe, src, 0x1000);
        if (R_SUCCEEDED(rc)) {
          u32 v = *(volatile u32 *)probe;
          Result u = svcUnmapMemory((void *)probe, src, 0x1000);
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx rc=0x%x read=0x%x unmap=0x%x WORKS=%d\n",
                      (unsigned long)probe, rc, v, u, v == 0xABCD1234);
          if (R_SUCCEEDED(u)) free(src);
        } else {
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx FAILED rc=0x%x\n",
                      (unsigned long)probe, rc);
          free(src);
        }
      }
    } else {
      debugPrintf("[heapprobe] no unmapped 256MB-aligned spot found in heap region\n");
    }
  }

  /* Arm the stack-region overcommit arena. The boot probe confirmed svcMapMemory
   * aliases heap pages into the stack region; Unity reserves ~2.8GB of PROT_NONE
   * pools but commits only ~80MB. Reserve a 1280MB stack-region window (cheap
   * address space) + a 256MB heap commit-pool; the OC arena (libc_shim.c) then
   * holds Unity's big reservations there and aliases pool pages in on mprotect.
   * Any failure leaves OC disabled and the engine runs on the heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);   // keep libnx thread stacks out
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES)) {
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p, heap-backed arena %u MB "
                    "(total reserve %u MB)\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool,
                    (unsigned)(g_mmap_arena_size >> 20),
                    (unsigned)((winsz + g_mmap_arena_size) >> 20));
        if (g_oc_win2 && g_oc_win2_sz) {
          virtmemLock();
          VirtmemReservation *rv2 = virtmemAddReservation(g_oc_win2, g_oc_win2_sz);
          virtmemUnlock();
          if (rv2 && oc_arena_add_window(g_oc_win2, g_oc_win2_sz))
            debugPrintf("[oc] ARMED window 2: %u MB @ %p (total window VA %u MB)\n",
                        (unsigned)(g_oc_win2_sz >> 20), g_oc_win2,
                        (unsigned)((winsz + g_oc_win2_sz) >> 20));
        }
      }
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole (win=%p sz=%u MB rv=%p) -> heap-backed only\n",
                  win, (unsigned)(winsz >> 20), (void *)rv);
    }
  }

  /* No screen-size override here. This is where the Zookeeper base forced a
   * PORTRAIT surface (720x1280 / 1080x1920) because Zookeeper is a TATE game;
   * PvZ Fusion is landscape, so those values were wrong for this port and only
   * survived because android_native_update_mode() overwrites them before
   * anything reads them. The panel size now has exactly one owner:
   * android_native_update_mode(), called at boot and once per frame. */

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  check_data();

  /* load the three modules; libil2cpp resolves its engine calls against libunity
   * module-to-module during relocation. */
  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  debugPrintf("[boot] loaded libmain   @ virtbase %p\n", (void *)main_mod.load_virtbase);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  debugPrintf("[boot] loaded libunity  @ virtbase %p\n", (void *)unity_mod.load_virtbase);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  debugPrintf("[boot] loaded libil2cpp @ virtbase %p\n", (void *)il2cpp_mod.load_virtbase);
  /* hand the il2cpp exec base to the GC stop-the-world bridge in libc_shim.c so
   * our pthread_kill can ack the Boehm GC's (undeliverable) suspend/restart
   * signals via its semaphore at il2cpp+0x41e28f8. */
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  /* Firebase native libs are intentionally NOT loaded. The first scene's
   * FirebaseManager only advances when the managed dependency check reports
   * DependencyStatus.Available(0); on a Switch there is no Google Play Services,
   * so the real libs could never report that (they return UnavailableMissing)
   * AND libFirebaseCppApp crashes our loader in its JNI_OnLoad logging path. We
   * instead answer the SDK's native P/Invoke lookups with stubs (firebase_stub.c
   * via dlsym_fake) that make the check resolve to Available. The 4 .so files can
   * be deleted from sdmc:/switch/zookeeper/. Firebase is cosmetic here (RemoteConfig
   * banner/news textures), so stubbing it costs only those images. */
  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed (canary-patch disabled)\n");

  /* Patch libunity AFTER finalize/flush -- the same point every other libunity
   * patch below runs at. so_patch_code aliases the target pages via
   * svcMapProcessMemory, but the module's segments must be finalized (mapped
   * with their final RX perms and relocated) first; doing it right after
   * load_module faulted at boot (fbstub94: stock .so on SD, PC in the patch
   * path before finalize). */
  nx_patch_libunity((uintptr_t)unity_mod.load_virtbase);

  /* Force FMOD to use its native OpenSL ES output instead of Unity's Java
   * AudioTrack driver.
   *
   * Mechanism (offsets in this paragraph are the Zookeeper 62f2 reference trace;
   * the PvZ 62f1c1 patch sites are the 0x7xxxxx offsets in the code below).
   * Unity's AudioManager FMOD init calls FMOD::System::setOutput with a
   * requested FMOD_OUTPUTTYPE in w1, derived at +0x6bea84 (mov w1,w21). FMOD's
   * setOutput walks the registered output list (+0xc80e84 loop) and matches the
   * requested type against each output's type field at output+0x78 (copied there
   * from the output description by the registrar +0xc744fc). The type constants,
   * read straight from each getDescriptionEx's desc+0x78:
   *     AudioTrack = 21 (0x15)   <- the default request; needs the JVM run loop
   *     OpenSL ES  = 22 (0x16)   <- callback-driven, self-driving via our shim
   * Default request is 21 (logged previously as "requested output: 21"), so the
   * Java AudioTrack output is selected and, with no JVM consumer, stays silent.
   *
   * fbstub63: rewrite the requested type at the setOutput call site from
   * "mov w1,w21" to "movz w1,#22", so Unity asks FMOD for OPENSL. FMOD finds the
   * registered OpenSL output (type 22), inits it -> dlopen(libOpenSLES.so) ->
   * slCreateEngine (our opensles.c shim) -> the engine drives its own callback
   * buffer queue. No Java handshake, correct lifecycle. The registration path is
   * left untouched (both AudioTrack and OpenSL register normally with their real
   * type fields). 0x2A1503E1 (mov w1,w21) -> 0x528002C1 (movz w1,#0x16). */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    uint32_t req_opensl = 0x528002C1u; /* movz w1, #22 (FMOD_OUTPUTTYPE OPENSL) */
    /* PvZ 62f1c1 offset 0x76afd4 (signature-matched from zk 0x6bea84). Verify the
     * expected `mov w1,w21` before writing -- belt-and-suspenders, cannot corrupt. */
    if (*(volatile uint32_t *)(ub + 0x76afd4) == 0x2A1503E1u) {
      so_patch_code((void *)(ub + 0x76afd4), &req_opensl, sizeof req_opensl);
      debugPrintf("[fmod] output forced to OpenSL(22) @libunity+0x76afd4\n");
    } else {
      debugPrintf("[fmod] SKIP force-OpenSL: +0x76afd4 = 0x%08x, not `mov w1,w21` -- "
                  "libunity differs from expected 62f1c1 (see PORTING sec 3)\n",
                  *(volatile uint32_t *)(ub + 0x76afd4));
    }
    /* Frame-pacing (Swappy) force-disable. PvZ registers Swappy (9 JNI entrypoints);
     * its init brings up a Choreographer/vsync-driven thread pool that never completes
     * on Switch -- there is no Android Choreographer to deliver frame callbacks -- so
     * engine-init parks in a pthread_join at frame 0 (verified on hardware: UnityMain
     * state=join for 18s, workers hard-parked in Swappy's 0xd6xxxx wait). libunity+
     * 0x6648d8 is the cached "is frame-pacing enabled?" getter: 13 call sites, each
     * `bl 0x6648d8 ; tbz w0,#0,<skip>`. Forcing it to return 0 makes every site take
     * the disabled path -> plain eglSwapBuffers, no pacing threads, no join. This is
     * how the Zookeeper base already boots (it never enables Swappy). Verify-first:
     * patch only if the prologue is the expected `stp x30,x19,[sp,#-0x10]!`. */
    if (*(volatile uint32_t *)(ub + 0x6648d8) == 0xA9BF4FFEu) {
      uint32_t off_pacing[2] = { 0x52800000u /* mov w0,#0 */, 0xD65F03C0u /* ret */ };
      so_patch_code((void *)(ub + 0x6648d8), off_pacing, sizeof off_pacing);
      debugPrintf("[pace] frame-pacing (Swappy) force-disabled @libunity+0x6648d8\n");
    } else {
      debugPrintf("[pace] SKIP Swappy-disable: +0x6648d8 = 0x%08x, not `stp x30,x19` -- "
                  "libunity differs (see PORTING)\n", *(volatile uint32_t *)(ub + 0x6648d8));
    }

    /* fbstub66: neutralise FMOD's OpenSL buffer-geometry validation.
     *
     * After slCreateEngine succeeds, FMOD's OpenSL init (+0xce67a0 -> continuation
     * +0xce6840) validates the output period against the DSP mixer buffer at
     * +0xce68d4..+0xce6924. It reads {sampleRate, framesPerBuffer} from the
     * AudioManager getProperty values (our jni_fake.c: 48000 / 64) and returns
     * FMOD error 60 ("Error initializing output device") if:
     *     sampleRate == 0, OR framesPerBuffer == 0, OR
     *     framesPerBuffer > (dspNumBuffers-1)*dspBufferLength  even after one halving.
     * dspNumBuffers (w20) / dspBufferLength (w21) come from the game's BAKED
     * AudioSettings (AudioSettings::GetDSPBufferSize, +0x646888) and pass straight
     * through the init wrapper +0xce63a8 unchanged. This title's baked buffer is
     * degenerate enough that even a 64-frame period fails the bound -- consistent
     * with dspNumBuffers == 1, which makes the bound (1-1)*len = 0 so NO positive
     * period can ever satisfy it. Reporting a smaller framesPerBuffer alone cannot
     * fix that case, so we also force the final bound check to pass.
     *
     * Patch the terminal compare-branch at +0xce6920 from "b.ls 0xce6930"
     * (0x54000089) to an unconditional "b 0xce6930" (0x14000004): the success path
     * at +0xce6930 then always runs and builds the audio player from the sane
     * sampleRate/channels. The buffer count it derives, N = (w20*w21)/period
     * (+0xce697c), stays >= 1 because we report a small 64-frame period. The two
     * zero-guards above (+0xce68ec/+0xce68fc) are left intact and pass (48000/64
     * are both non-zero). */
    uint32_t b_uncond = 0x14000004u; /* b +0x10 (was b.ls, 0x54000089) -- same local target */
    /* PvZ 62f1c1 offset 0xe2cfb8 (signature-matched from zk 0xce6920). Verify a real b.ls. */
    if (*(volatile uint32_t *)(ub + 0xe2cfb8) == 0x54000089u) {
      so_patch_code((void *)(ub + 0xe2cfb8), &b_uncond, sizeof b_uncond);
      debugPrintf("[fmod] OpenSL buffer-geometry check bypassed @libunity+0xe2cfb8\n");
    } else {
      debugPrintf("[fmod] SKIP buffer-geometry bypass: +0xe2cfb8 = 0x%08x, not b.ls -- "
                  "libunity differs from expected 62f1c1 (see PORTING sec 3)\n",
                  *(volatile uint32_t *)(ub + 0xe2cfb8));
    }
  }


  /* The main thread runs init_array + the engine lifecycle; give it its own
   * stable bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  /* fake JNI + our environment, then HID */
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  /* resolve UnityPlayer natives (load_virtbase + recovered offsets) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);
  debugPrintf("[boot] entry points resolved (initJni=%p render=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender);

  /* re-assert the guard right before handing control to the engine, so no
   * intervening libnx/jni setup left tpidr in an unexpected state */
  install_bionic_tls(main_tls);

  /* drive the lifecycle the Java UnityPlayer would */
  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST. It runs jni::Initialize(),
   * which caches the JavaVM into libunity's internal JNI manager; without this
   * ScopedJNI/LocalScope inside initJni get a NULL JNIEnv and crash. It also
   * AttachCurrentThread()s and RegisterNatives() for each subsystem (our fake
   * env handles FindClass/RegisterNatives as safe no-ops). */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    debugPrintf("[boot] calling JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime. il2cpp caches the VM in a
   * global it later checks; without it, il2cpp logs "Java VM not initialized"
   * and every managed AndroidJNI / AndroidJavaObject call (the Twitter SDK +
   * the SWIG-wrapped AppUtil module the first scene initializes) fails, hanging
   * scene load.
   *
   * We do NOT call libil2cpp's JNI_OnLoad: its first action is a log via
   * __android_log_print, whose GOT slot in libil2cpp is mis-bound (resolves to
   * a heap address -> Instruction Abort). Its only *essential* effects are two
   * global stores (verified by disassembling THIS Daggerfall libil2cpp's
   * JNI_OnLoad @ 0x1b87740): cache the VM at il2cpp+0x45c20c8, and store the JNI
   * handler fn-ptr (il2cpp+0x1b87784, which the reg-fn @0x1c1c8f0 writes) at
   * il2cpp+0x45c3cc0. Both land in the RW segment. Replicate the two stores.
   * See dfu_offsets.h for the full derivation. */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    *(void **)(b + OFF_il2cpp_VM_global) = fake_vm;
    *(void **)(b + OFF_il2cpp_handler_ptr_store) =
        (void *)(b + OFF_il2cpp_JNI_handler_fn);
    debugPrintf("[boot] il2cpp JavaVM global set (vm=%p)\n", fake_vm);
    nx_boot_il2cpp_hacks();   /* install Time.get_* hooks; GC handled by bridge */
#if DFU_PAD_BRIDGE
    dfu_input_init();         /* guard-check + resolve the action injection API */
#else
    debugPrintf("[input] Tier 2 pad bridge DISABLED (DFU_PAD_BRIDGE=0, config.h)"
                " -- isolating the Il2CppExceptionWrapper abort. The game's own"
                " on-screen touch controls (Tier 1) are unaffected.\n");
#endif
  }

  /* (Firebase native libs are not loaded; their JNI_OnLoad is neither needed nor
   * safe to call -- see the boot-time note above. The managed SDK's native calls
   * are answered by firebase_stub.c through dlsym_fake.) */

  debugPrintf("[boot] calling initJni...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] initJni returned; nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  debugPrintf("[boot] gfx state created; sendSurfaceChanged...\n");
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface change sent; resuming + focusing player loop\n");

  /* CRITICAL: on Android the Unity player loop only advances Update/coroutines/
   * animation when the app is RESUMED and FOCUSED. The Java UnityPlayer drives
   * this from onResume()/onWindowFocusChanged(). We had been calling only
   * initJni + gfx + render, so the engine stayed paused: it loaded the boot
   * scene and ran Awake/Start once (hence Firebase init), then rendered a frozen
   * frame forever without ticking a single Update or coroutine -- which is why
   * StartInitializer.InitUpdate was never called. Issue the resume + focus
   * transitions the lifecycle normally would before the render loop. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  debugPrintf("[boot] resumed + focus=true; entering render loop\n");
#if DFU_HAVE_TIME_FIX
  nx_install_time_fix();   /* PvZ: hook Update + start clock thread BEFORE the first (blocking) nativeRender */
#endif

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");   // the thread that drives nativeRender
  /* Watchdog re-enabled (fbstub88) with the snapshot-ordering fix: stacks are
   * now walked while the target thread is PAUSED (the fbstub86 self-crash came
   * from resuming first and walking a live stack). 6s thresholds. Its job now:
   * catch the first-present hang and dump the thread blocked in eglSwapBuffers. */
  diag_watchdog_start();

  int frame = 0;

  /* --- TRUE 1080p FIX: track dock state ---
   * android_native_update_mode() already swaps g_w/g_h (and screen_width/
   * screen_height) between 1280x720 and 1920x1080 every frame, but Unity caches
   * its surface dimensions and only re-reads them on a surface-changed event.
   * Without one, docking mid-game leaves the engine rendering at the old size.
   * Seed the tracker here so the first iteration doesn't spuriously fire. */
  AppletOperationMode current_op_mode = appletGetOperationMode();

  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);   // heartbeat: lets the watchdog see progress (or its absence)
    nx_time_tick();      // advance our managed-Time clock once per frame

    /* Pad -> Daggerfall actions. MUST stay on this thread: SetKey/SetAxis are
     * managed methods and need the il2cpp-attached render thread (see
     * dfu_input_offsets.h "THREADING"). This only ADDS action state, so the
     * game's own on-screen touch controls keep working alongside it. */
#if DFU_PAD_BRIDGE
    dfu_input_pump();
#endif
    /* Software keyboard: RENDER THREAD ONLY -- swkbdShow() blocks in the system
     * applet. Outside the DFU_PAD_BRIDGE guard on purpose (see dfu_keyboard.c). */
    dfu_kbd_tick();
    /* fbstub42: the engine clock is now fixed at its true source by the
     * TimeManager::Update entry hook (installed at boot, see nx_install_time_fix):
     * Update is re-driven each frame with newTime = GetTimeSinceStartup(), so all
     * deltaTime variants advance and the PreloadManager can integrate. No per-frame
     * field poking needed here. */
    android_native_update_mode();

    /* --- TRUE 1080p FIX: alert Unity when you dock/undock ---
     * Must run AFTER android_native_update_mode() so the new dimensions are
     * already published when Unity re-queries the display during the event. */
    AppletOperationMode new_op_mode = appletGetOperationMode();
    if (new_op_mode != current_op_mode) {
      current_op_mode = new_op_mode;
      debugPrintf("[gfx] operation mode -> %s (%ux%u); sending surfaceChanged\n",
                  new_op_mode == AppletOperationMode_Console ? "docked" : "handheld",
                  android_native_width(), android_native_height());
      /* forces Unity to update its UI boundaries to 1920x1080 mid-game */
      Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
    }
    /* -------------------------------------------------------- */

    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame == 0) {
      /* fbstub42: install the native engine-clock fix first thing. Drives
       * TimeManager::Update with a live newTime so deltaTime / m_Time advance for
       * native readers (PreloadManager), unblocking async scene loads. */
      /* nx_install_time_fix() moved before the render loop (installs too late here:
       * PvZ's first nativeRender blocks on the scene load and never returns). */
      /* Time.get_* hooks are installed at boot (nx_boot_il2cpp_hacks, right after
       * the JavaVM global is set). This call is a self-skipping backstop only. */
      nx_boot_il2cpp_hacks();
    }
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
#if DFU_HAVE_FINISH_PROBE
    if (frame == 90 || frame == 300 || frame == 600 || frame == 1200)
      nx_probe_finish((uintptr_t)il2cpp_mod.load_virtbase);
#endif
    frame++;
  }

  /* Commit any sensitivity change made inside the last 3 seconds -- the pointer
   * normally debounces its own save, so quitting quickly after a D-pad tweak
   * would otherwise lose it. */
  android_native_input_shutdown();

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  /* Print the JNI approximation ledger on the way out: which fakes were hit,
   * and which of them Unity actually read back. See config.h DFU_JNI_LOUD. */
  jni_approx_summary("clean shutdown");

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
