/* config.h -- Daggerfall Unity Switch wrapper configuration
 * (forked from the PvZ Fusion 3.8.1 wrapper config, itself from Zookeeper DX.)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/* ============================ MEMORY LAYOUT ==============================
 * These are engine-fitting parameters, not game content. The starting values
 * are inherited from PvZ Fusion because it is the SAME Unity minor version
 * (2022.3.62) and we apply the SAME 256MB->64MB region-granularity patch (see
 * nx_patch_dfu.h).
 *
 * !! READ THIS BEFORE YOU TOUCH ANYTHING ELSE !!
 *
 * Daggerfall Unity is a MUCH heavier memory client than PvZ Fusion, and these
 * numbers are the single most likely thing to need changing. PvZ's tuning was
 * balanced against an observed ~551 MB live working set on a bounded scene.
 * Daggerfall streams an open world, keeps large texture and terrain caches, and
 * has a mod loader; its working set is larger and, unlike PvZ's, it GROWS as
 * you travel rather than settling after the first scene.
 *
 * So treat every number below as a STARTING POINT, not a known-good value.
 * The good news is the failure mode is legible: each arena logs which knob
 * starved when it OOMs. Bring the port up with DEBUG_LOG 1 and read the log.
 *
 * The budget is roughly (newlib heap + mmap arena + OC pool) <= ~2945 MB in
 * title-override mode. You cannot simply raise everything; growing one shrinks
 * the others. PvZ's history, kept because the failure points are informative:
 *   - OC pool at 256 MB exhausted during scene load -> hard OOM
 *   - mmap arena at 512 MB could not fit a 127 MB overflow -> mmap NULL -> crash
 *   - mmap arena granularity at 16 MB CORRUPTED the Dynamic Heap allocator at
 *     init (overlapping regions from the over-map/trim pattern). 64 MB is the
 *     known-good floor and must match the libunity patch. Do not lower it.
 * ======================================================================== */

// The engine + libc++ + il2cpp heap need a generous newlib heap; the rest of
// system memory is handed to the .so loader (see __libnx_initheap).
#define MEMORY_MB 768

// Anonymous-mmap arena. Unity reserves big region-aligned pools by over-mmapping
// then munmapping the unaligned head/tail. We back anonymous mmaps from a
// dedicated, region-aligned arena with a per-page used-bitmap so sub-range
// munmap frees exactly the trimmed pages.
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)    // MUST match the libunity patch (nx_patch_dfu.h). 16MB corrupts the allocator; do not lower.
/* Heap-backed mmap arena. RAISED 192 -> 320 MB on evidence, not on feel: the
 * FMV-to-dungeon run filled it to 189 MB of 192 --
 *     [mmap] fallback 19736 KB -> ... (total 189 MB)
 * -- i.e. 98%, one more fallback from the "mmap NULL during world load" this
 * comment used to only warn about. Must stay a multiple of MMAP_ARENA_ALIGN. */
#define MMAP_ARENA_RESERVE  ((size_t)320 * 1024 * 1024)

// Stack-region overcommit (OC) arena (see libc_shim.c): PROT_NONE reservations
// held in a stack-region window, committed pages backed from a small heap pool.
#define OC_WINDOW_BYTES     ((size_t)2048 * 1024 * 1024)  // cheap PROT_NONE reservation; the window-finder clamps to the largest stack-region hole.
// Commit-pool: real memory backing touched pages of the OC window. Unity is told
// it has 512 MB (libc_shim.c __sysconf PHYS_PAGES + dalvik.vm.heapsize), reserves
// its heaps as big PROT_NONE regions that route here, so this pool must be able
// to back what Unity actually touches. This is the knob Daggerfall is most
// likely to exhaust -- watch for it growing as you fast-travel.
/* Lowered 896 -> 768 to pay for the arena growth above, because total reserve
 * is bounded and this had the slack: the same run peaked at 662 MB of 896
 * ("[oc] committed 253 MB (pool 662/896 MB ...)"), so 768 still leaves ~106 MB
 * of headroom over the observed high-water mark. If a later log shows the pool
 * near 768, take the next 128 MB from the GPU arena rather than from here. */
#define OC_POOL_BYTES       ((size_t)768 * 1024 * 1024)

// Overcommit (alias-region) mode: reserve a big *virtual* window (PROT_NONE
// costs only address space) and commit physical pages on demand -- true
// overcommit, matching Android.
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)  // 6 GB virtual reservation window
#define OVERCOMMIT_HEAP_MB  608u                          // newlib malloc + .so load zone

/* ============================ GAME IDENTITY =============================== */

// Daggerfall Unity ships the engine as the standard modern Unity trio:
// libmain.so dlopens libunity.so which dlopens libil2cpp.so. main.c loads all
// three directly, so these SO_NAME macros are kept only for parity with the base.
#define SO_NAME      "libunity.so"
#define SO_CPP_NAME  "libil2cpp.so"

// The SD-card folder holding the .nro + the game files.
/* The folder name is now only a FALLBACK, used if the runtime scan finds
 * nothing (nx_data_root.c step 6). It is no longer load-bearing: name the
 * folder anything, put it anywhere on the card. */
#define GAME_FOLDER  "daggerfall"

/* LOG_NAME and GAME_HOME are GONE. Both were compile-time paths built from
 * GAME_FOLDER, and both were load-bearing in the worst way: name the folder
 * anything else and the loader could not find libmain.so, and could not write
 * a log to say so, because the log lived under the same wrong root.
 *
 * Use instead, from nx_data_root.h:
 *     g_data_root      the resolved folder      (was GAME_HOME)
 *     g_log_path       <root>/debug.log         (was LOG_NAME)
 *     nx_path("/sub")  <root>/sub               (was GAME_HOME "/sub")
 *
 * Do not reintroduce them. If you need a path, it is a runtime value. */

/* getenv("HOME") / getpwuid()->pw_dir now return g_data_root (libc_shim.c).
 *
 * This matters here: Daggerfall Unity writes real files -- saves, settings.ini,
 * its own log -- through managed System.IO, deriving the paths from
 * Application.persistentDataPath, which lands under HOME. Saves therefore go to
 * the same folder as the .nro, whatever it is called. */

// flip to 1 (and rebuild) to get file logging (debug.log) for on-hardware debugging
#define DEBUG_LOG 0   /* ON: leave it on for the whole bring-up */

/* High-volume per-operation traces. Invaluable for black-screen / boot-hang
 * triage, catastrophic for load speed once the game runs: every data.unity3d
 * read/lseek and most mprot calls fflush two lines to the SD card. Daggerfall
 * loads far more bundles than PvZ did, so leaving these on will look like a
 * hang. Keep them OFF for normal play. */
/* How often debugPrintf is allowed to hit the SD card, in milliseconds. Each
 * flush is a synchronous write; flushing per line cost ~11,000 of them a run
 * and was a visible source of stutter. The crash handler flushes explicitly, so
 * this only bounds what a hard HANG could lose. */
#define LOG_FLUSH_MS    250

/* Per-open file trace ("[io] open(...)"). 2181 lines in a normal run, one
 * synchronous formatting call per game file open, and Daggerfall opens BSA
 * files constantly. Off by default; turn on to debug a missing-file problem. */
#define TRACE_FILE_IO   0

#define TRACE_BUNDLE_IO 0   /* per-read/lseek trace of data.unity3d */
#define TRACE_MPROT     0   /* per-mprotect commit trace */

/* JNI approximation ledger (jni_fake.c, adopted from killerbean_nx).
 *
 * Records every distinct place the fake JNI returns a plausible LIE -- empty
 * string, null object, zero, no-op -- counts hits, and marks the ones Unity ran
 * ExceptionCheck against immediately afterwards. Those INSPECTED entries are
 * the fakes whose wrong answer becomes a stored value inside the engine; the
 * rest are calls nobody reads. That is the difference between the forty things
 * we fake and the three that matter.
 *
 * ON by default here, unlike the reference ports, because this port has not
 * booted yet and PORTING.md sec 8 says the first run's goal is a log rather
 * than a game. Cost is one compare in the dispatch hot path plus one line per
 * NEW site (repeat hits are counted, not printed). Set to 0 for release; the
 * ledger is also fully suppressed when DEBUG_LOG is 0. */
#define DFU_JNI_LOUD 1

/* Adopted with the clonehero_nx JNI layer. DFU_JNI_LEDGER gates the
 * approximation ledger (was DFU_JNI_LOUD's job before the swap; both are on).
 * DFU_JNI_QUARANTINE is the local-ref quarantine depth: a freed ref is held in
 * a ring and released only once this many more have been retired, so a
 * use-after-free reads stale data instead of reallocated memory. */
/* TIER 2 PAD BRIDGE -- BACK ON. The isolation experiment is finished and it
 * cleared this code.
 *
 * sec 16 turned it off to find out whether it was the source of
 *     terminating with uncaught exception of type Il2CppExceptionWrapper
 * and wrote down what each outcome would mean: "game survives -> the bridge was
 * the source"; "still aborts -> the bridge is innocent, turn it back on."
 *
 * The abort still occurs with DFU_PAD_BRIDGE=0. So the bridge is innocent, and
 * leaving it off was costing every button and stick on the console for nothing
 * -- with it off the only input is nx_pointer's cursor emulation (A/ZR/ZL tap,
 * D-pad scroll, stick moves a pointer), which is exactly the "most of the
 * buttons and sticks don't work" being reported.
 *
 * The unsafe-by-construction concern from sec 16 stands and is NOT fixed:
 * SetKey/SetAxis/TriggerAction are managed methods called from a C frame, and a
 * C frame cannot catch the C++ exception IL2CPP raises. il2cpp_runtime_invoke
 * (exported at 0x1bd7a2c) RETURNS the exception instead of throwing it and is
 * the proper firewall, but it takes a MethodInfo*, and this port has raw RVAs
 * rather than MethodInfo pointers -- so adopting it is real work, not a
 * one-liner, and it is not what is failing today. */
/* Longest name the software keyboard will accept. Daggerfall's own character
 * name box caps well below this; the applet just needs an upper bound. */
#define DFU_KBD_MAXLEN     32

#define DFU_PAD_BRIDGE     1

#define DFU_JNI_LEDGER     1
#define DFU_JNI_QUARANTINE 512

/* ------------------------- NO RUNTIME CONFIGURATION -----------------------
 * There is no config.txt: nothing is read from the SD card and nothing is
 * written to it. Everything above is a build-time constant, deliberately.
 *
 * The two globals below are the render size in use right now. They are NOT
 * settings: config.c gives them the handheld panel size as a starting value and
 * android_native_update_mode() re-derives them from the docked/handheld state
 * every frame.                                                              */
extern int screen_width;
extern int screen_height;

#endif
