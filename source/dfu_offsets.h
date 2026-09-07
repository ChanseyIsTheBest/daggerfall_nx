/* dfu_offsets.h -- every libil2cpp.so address this port writes to or reads
 * from, for DAGGERFALL UNITY (Unity 2022.3.62f3, arm64, IL2CPP).
 *
 * Target binary these offsets were derived from:
 *     libil2cpp.so   BuildID sha1 cc72326dbdd256d9043b0690a3da85eea77f0cda
 *                    73,037,032 bytes
 *
 * The PvZ base scattered these constants across main.c and libc_shim.c. They
 * are collected here instead, because they are the single most build-specific
 * thing in the tree and the most expensive to get wrong: unlike the libunity
 * allocator table (which is verify-first and fails loudly), MOST OF THESE ARE
 * BLIND WRITES. A wrong address is silent memory corruption with no diagnostic.
 *
 * Run `python3 tools/verify_offsets.py <libunity.so> <libil2cpp.so>` before the
 * first boot on hardware. It re-derives every constant below from the binaries
 * and diffs them against this header.
 *
 * ---------------------------------------------------------------------------
 * DERIVATION -- all from YOUR binaries, none inherited from PvZ
 * ---------------------------------------------------------------------------
 *
 * ALL ADDRESSES BELOW ARE LINK-TIME VADDRS (runtime = load_virtbase + off),
 * not file offsets. For libil2cpp the two differ by 0x4000.
 *
 * Time.get_* RVAs: read out of dump.cs (UnityEngine.Time, TypeDefIndex 8981).
 *   Every one of the seven hooked getters was confirmed to start with
 *   0xA9BF4FFE (`stp x30,x19,[sp,#-0x10]!`) -- the guard word the PvZ notes
 *   recommend checking. set_timeScale opens differently (0xFC1E0FE8,
 *   `str d8,[sp,#-0x20]!`) because it takes a float argument; that is correct,
 *   not a mismatch.
 *
 * JNI globals: libil2cpp exports JNI_OnLoad, so this was read straight off its
 *   disassembly:
 *       0x1b87740  stp x30,x19,[sp,#-0x10]!
 *       0x1b87764  adrp x8, #0x45c2000
 *       0x1b8776c  str  x19, [x8, #0xc8]      <- VM global = 0x45c20c8
 *       0x1b87768  add  x0, x0, #0x784        <- handler fn = 0x1b87784 (entry+0x44)
 *       0x1b87770  bl   #0x1c1c8f0            <- registration fn
 *   and the registration fn is the expected three-instruction shape:
 *       0x1c1c8f0  adrp x8, #0x45c3000
 *       0x1c1c8f4  str  x0, [x8, #0xcc0]      <- handler-ptr storage = 0x45c3cc0
 *       0x1c1c8f8  ret
 *   The handler being at entry+0x44 matches both PvZ 3.6.1 and 3.8.1.
 *
 * GC bridge: exactly TWO pthread_kill call sites exist in the whole binary
 *   (found via the .rela.plt GOT slot -> PLT stub -> BL scan), so these are
 *   unambiguous. Signal numbers are read at the call sites:
 *       0x1c36e58  ldr w1,[x24,#0xa6c]   x24 = 0x45b2000  -> suspend = 0x45b2a6c
 *       0x1c370c0  ldr w1,[x24,#0xa70]   x24 = 0x45b2000  -> restart = 0x45b2a70
 *       0x1c370a4  ldr w8,[x23,#0xa68]   x23 = 0x45b2000  -> ack gate = 0x45b2a68
 *   Both sem_post sites pass x0 = 0x47d6c40, and the second is gated by
 *   `ldr w8,[x8,#0xa68] ; cbz` -- exactly the shape pthread_kill_gc() replicates
 *   in libc_shim.c. Same structure PvZ documented, different addresses.
 */
#ifndef DFU_OFFSETS_H
#define DFU_OFFSETS_H

/* ---- expected identity of the binaries these offsets describe -------------
 * verify_offsets.py checks these; main.c logs them at boot.                */
#define DFU_UNITY_VERSION      "2022.3.62f3"
#define DFU_UNITY_BUILDID      "1dc173bbf7a97f88"                          /* xxHash */
#define DFU_IL2CPP_BUILDID     "cc72326dbdd256d9043b0690a3da85eea77f0cda"  /* sha1   */
#define DFU_UNITY_SIZE         18023304u
#define DFU_IL2CPP_SIZE        73037032u

/* ---- UnityEngine.Time getters (libil2cpp RVA) -----------------------------
 * All seven guard-checked: first word must be 0xA9BF4FFE before hooking.   */
#define DFU_TIME_GUARD_WORD            0xA9BF4FFEu

#define OFF_Time_realtimeSinceStartup  0x3e7cc38
#define OFF_Time_time                  0x3e7f39c
#define OFF_Time_timeSinceLevelLoad    0x3e7f3c4
#define OFF_Time_deltaTime             0x3e7f3ec
#define OFF_Time_fixedTime             0x3e7f414
#define OFF_Time_unscaledTime          0x3e7f43c
#define OFF_Time_unscaledDeltaTime     0x3e7f464
#define OFF_Time_fixedDeltaTime        0x3e7f48c
#define OFF_Time_set_fixedDeltaTime    0x3e7f4b4   /* guard 0xFC1E0FE8        */
#define OFF_Time_smoothDeltaTime       0x3e7f4ec
#define OFF_Time_timeScale             0x3e7f514

/* THE ICALL CACHE SLOT FOR get_timeScale. This is not decoration -- calling the
 * stub at OFF_Time_timeScale before IL2CPP is initialised CRASHES THE GAME, and
 * did. See AUDIT.md sec 15.
 *
 * get_timeScale is not the getter. It is a lazy internal-call resolver:
 *
 *     ldr  x0, [x19, #0x658]     ; cached pointer, .bss, zero at rest
 *     cbnz x0, use_it
 *     adrp/add x0, "UnityEngine.Time::get_timeScale()"
 *     bl   ResolveICall
 *     str  x0, [x19, #0x658]     ; caches the answer -- INCLUDING NULL
 *     br   x0                    ; tail-call; branches to 0 if it failed
 *
 * Called before the icall table is populated, ResolveICall fails, the runtime
 * goes to throw System.MissingMethodException, and the corlib image it needs is
 * still NULL -- so it faults reading [NULL+0x30] while building the exception.
 * The crash is in the error reporting, not in the lookup, which is why the
 * register dump showed "System" and "MissingMethodException" rather than
 * anything to do with time.
 *
 * So do NOT call the stub. Read this slot instead: if the GAME has already
 * resolved get_timeScale, the slot holds the real function and calling it is
 * safe; if it is still zero, the answer is simply not available yet and
 * timeScale stays 1.0. We never trigger a resolution ourselves.
 *
 * Derived from the stub's own adrp+ldr pair (0x45bf000 + 0x658), not typed --
 * tools/verify_offsets.py re-derives it the same way. */
#define OFF_Time_timeScale_ICACHE      0x45bf658
#define OFF_Time_set_timeScale         0x3e7f53c   /* guard 0xFC1E0FE8, not the above */
#define OFF_Time_frameCount            0x3e7f574
#define OFF_Time_renderedFrameCount    0x3e7f59c

/* Coverage of UnityEngine.Time in THIS build -- 14 methods, audited by
 * disassembling every call site (tools/xcall.py), not by reading dump.cs.
 *
 * Bouncemasters (Unity 6000) reached 15/15. Four of the five it initially
 * missed -- get_timeAsDouble, get_realtimeSinceStartupAsDouble,
 * get_timeAsRational, get_timeAsRational_Injected -- have 0 occurrences in
 * THIS game's dump.cs, so they are not hookable here and the target is 14.
 *
 * CORRECTION: an earlier version of this comment said those members "do not
 * exist in 2022.3.62" and were "Unity 6000 additions". That explanation is
 * WRONG. clonehero_nx is also Unity 2022.3.62f3 and its UnityEngine.Time
 * exposes 13 accessors INCLUDING get_timeAsDouble and
 * get_realtimeSinceStartupAsDouble. The real reason they are absent here is
 * that IL2CPP STRIPS UNUSED MEMBERS PER BUILD: Daggerfall's managed code never
 * reads them, so they were stripped; Clone Hero's does, so they survived.
 * The count is a property of the game, not of the Unity version.
 *
 * This matters beyond bookkeeping. AArch64 returns float in s0, double in d0,
 * int in w0, and the PvZ base used a float-returning thunk for every time-like
 * accessor. Hooking a double-returning accessor with a float thunk leaves d0
 * holding whatever was there -- garbage, surfacing much later and looking like
 * a game bug. Daggerfall is safe ONLY because it has no double accessors;
 * verified, not assumed (all five checked, 0 occurrences each). If a future
 * build of the game starts using Time.timeAsDouble, this hook table needs a
 * double-returning thunk, not another entry pointing at nx_time_f.
 *
 *   HOOKED (8) -- answered from the wrapper clock:
 *     time, deltaTime, unscaledTime, unscaledDeltaTime, smoothDeltaTime,
 *     frameCount, renderedFrameCount, realtimeSinceStartup
 *
 *   NOT HOOKED (6) -- deliberately left to the driven native TimeManager:
 *     timeSinceLevelLoad (1 caller)   needs a level-load reset the wrapper
 *                                     does not observe; the native manager does
 *     fixedTime (2), fixedDeltaTime (2), set_fixedDeltaTime (3)
 *                                     physics clock; FixedUpdate is Unity's,
 *                                     and TimeManager::Update is already driven
 *     timeScale (3), set_timeScale (7) OWNED BY THE GAME -- see below
 *
 * timeScale is the one that matters. GameManager::PauseGame writes it, and
 * Daggerfall pauses on every inventory screen, menu and rest. The first cut of
 * this port bound deltaTime, unscaledDeltaTime AND smoothDeltaTime to the same
 * raw wall-clock delta, so scaled and unscaled were identical and timeScale was
 * ignored entirely: setting timeScale = 0 would have paused nothing and the
 * world would have kept running under every menu. That is exactly the failure
 * Bouncemasters PORTING sec 27 describes.
 *
 * Fixed by reading, not mirroring: nx_time_tick() calls the unhooked
 * get_timeScale each frame and scales deltaTime/time/smoothDeltaTime by it,
 * leaving the unscaled pair raw. The game stays the single source of truth and
 * there is no setter to trampoline. */

/* ---- libil2cpp JNI globals (blind writes -- see header note) -------------- */
#define OFF_il2cpp_JNI_OnLoad          0x1b87740   /* exported                     */
#define OFF_il2cpp_JNI_handler_fn      0x1b87784   /* entry + 0x44                 */
#define OFF_il2cpp_VM_global           0x45c20c8   /* store the fake JavaVM* here  */
#define OFF_il2cpp_handler_ptr_store   0x45c3cc0   /* what the reg fn writes       */
#define OFF_il2cpp_registration_fn     0x1c1c8f0   /* adrp / str x0 / ret          */

/* ---- Boehm GC stop-the-world bridge (read by libc_shim.c) ----------------- */
#define GC_SUSPEND_SIG_OFF             0x45b2a6c   /* GC_suspend_all pthread_kill arg */
#define GC_RESTART_SIG_OFF             0x45b2a70   /* GC_start_world pthread_kill arg */
#define GC_START_ACK_OFF               0x45b2a68   /* restart ack gate                */
#define GC_ACK_SEM_OFF                 0x47d6c40   /* FakeSem* ack-sem storage        */

/* ---- libunity.so, non-table sites ----------------------------------------
 * The 21-word allocator table and the BufferGLES force live in nx_patch_dfu.h
 * (they are verify-first). BufferGLES::BeginWrite was independently confirmed
 * by symbol fingerprinting: ref _ZN10BufferGLES10BeginWriteEmm -> game 0x9df3e8,
 * an exact whole-function match, agreeing with the caps-read derivation.     */
#define OFF_unity_JNI_OnLoad           0x6738e4    /* exported; we do NOT call it  */
#define OFF_unity_BufferGLES_BeginWrite 0x9df3e8   /* fn entry; caps site = +0x2c  */


/* ===========================================================================
 * libunity.so, FROM SYMBOLS
 * ---------------------------------------------------------------------------
 * Everything below was derived against a VERSION-MATCHED reference pair:
 *
 *     libunity_sym.so        BuildID sha1 26c9375b94e76ed44a541f424f1b57398d5d5810
 *                            symbols-only (.text NOBITS), 78,038 FUNC symbols
 *     libunity2022362f3.so   same BuildID, the bytes at those addresses
 *
 * That pair is Unity 2022.3.62f3 -- the SAME version as the game's libunity.so,
 * but a DIFFERENT build (the game's BuildID is xxHash 1dc173bbf7a97f88). So the
 * symbol addresses are NOT the game's addresses and were never used as such.
 *
 * METHOD: masked-instruction fingerprinting (tools/symmap.py, symfuzz.py).
 * Take the named function's bytes from the reference, mask only the fields that
 * MUST differ between two builds -- ADRP/ADR immediates, B/BL targets, and
 * literal-pool LDR displacements -- and search the game's .text for the rest.
 * Opcodes, registers, struct field offsets and move-wide constants must match
 * exactly. Same Unity version means the compiler emitted the same code; only
 * the linker moved it.
 *
 * The method was validated on two functions whose game addresses were already
 * known by independent means before any symbol file existed:
 *     JNI_OnLoad            -> 0x6738e4  (matches .dynsym)
 *     nativeUnitySendMessage-> 0x673440  (matches the .rela.dyn JNINativeMethod
 *                                         table extraction)
 * Both reproduced exactly. Every address below was then confirmed by reading
 * the disassembly side by side with the named reference function.
 *
 * Note the 62f2 kit (libunity_symbols_kit.7z, BuildID 78b00746...) is a
 * DIFFERENT pair for a different game and its addresses are not ours. Its
 * HOWTO_SYMBOLS.md describes this method and is worth keeping for that.
 * =========================================================================== */

/* ---- Time -----------------------------------------------------------------
 * TimeManager::Update(double), 456 bytes, matched 94.74% -- the only candidate,
 * at a function entry. Every mismatched word is an adrp-paired float literal
 * load, i.e. link-layout, not code. Verified instruction-for-instruction:
 *   0x4bfe14  ldr x8,[x0,#0xc8]    m_FrameCount
 *   0x4bfe18  ldr w9,[x0,#0xd0]
 *   0x4bfe1c  ldrb w10,[x0,#0xf8]  pause
 *   ...
 *   0x4bfe38  ldr d2,[x0,#0xe8]    m_StartupRef   <- body, entry+0x24
 * Struct field offsets are identical to the reference, as expected for one
 * Unity version. Body at entry+0x24 matches PvZ Fusion exactly.             */
#define OFF_TimeManager_Update        0x4bfe14
#define OFF_TimeManager_Update_body   0x4bfe38   /* entry + 0x24 */
#define DFU_TIMEMGR_GUARD_WORD        0xF9406408u /* ldr x8,[x0,#0xc8] */

/* GetTimeManager() is an 8-byte thunk (`mov w0,#7 ; b GetManagerFromContext`),
 * far too short to fingerprint -- the body is two generic instructions. Found
 * structurally instead (tools/xref.py thunks): Unity's subsystem getters form a
 * FAMILY of `mov w0,#id ; b X` thunks. The reference has 20 of them pointing at
 * GetManagerFromContext, with ids {1..14,16,17,19..22} and TimeManager = id 7.
 * Scoring the game's thunk families by id-set overlap picks one family (14/20
 * ids, all a subset of the reference's) whose id-7 member is 0x4c0448.
 *
 * The game did not ICF-merge GetManagerFromContext, so there are two identical
 * copies (0x40baa0 / 0x40bab0) reading the SAME manager table at 0x1158848, and
 * hence two id-7 thunks. They are byte-identical and interchangeable.        */
#define OFF_GetTimeManager            0x4c0448
#define OFF_GetManagerFromContext     0x40baa0
#define OFF_ManagerTable              0x1158848  /* ldr x0,[table, w0, sxtw #3] */

/* ---- Swappy ---------------------------------------------------------------
 * Swappy::IsEnabledAndActive(), 96 bytes, best candidate 79.17% against a 12.50%
 * runner-up. All five mismatches are ldrb/strb displacements into the Swappy
 * state globals (reference page 0x187f000 -> game 0x11ac000); the struct field
 * read `ldrb w8,[x0,#0x49a]` is IDENTICAL, as are all 24 instruction shapes.
 *
 * We drive our own render loop, so Swappy frame pacing must be inert. Patch the
 * entry to `mov w0,#0 ; ret`. PvZ Fusion forced this too.                     */
#define OFF_Swappy_IsEnabledAndActive 0x649b0c
#define DFU_SWAPPY_GUARD_WORD         0xA9BF4FFEu /* stp x30,x19,[sp,#-0x10]! */

/* ---- Vsync ----------------------------------------------------------------
 * WaitVSync(long), 96 bytes, 87.50% vs a 25% runner-up; 24/24 instruction
 * shapes identical. Its whole body is the state we need:
 *
 *   0x667c8c  adrp x20,#0x11b1000 ; add x20,x20,#0x50   -> mutex   0x11b1050
 *   0x667c9c  bl   pthread_mutex_lock
 *   0x667ca4  ldr  x21,[x22,#0xa8]                      -> counter 0x11b10a8
 *   0x667ca8  cmp  x21,x19 ; b.ge out
 *   0x667cb0  add  x0,x20,#0x28                         -> cond    0x11b1078
 *   0x667cb8  bl   pthread_cond_wait ; b back to the load
 *
 * So: a 64-bit counter, a mutex and a condvar. Bouncemasters' guidance applies
 * -- PUMP THE COUNTER, DO NOT PATCH THE WAIT. Bump the counter under the mutex
 * and signal the condvar once per presented frame.                            */
#define OFF_WaitVSync                 0x667c80
#define OFF_VSYNC_MUTEX               0x11b1050
#define OFF_VSYNC_COND                0x11b1078   /* mutex + 0x28 */
#define OFF_VSYNC_COUNTER             0x11b10a8   /* uint64_t                */

/* Unity's Android frame counter (s_FrameCounter, uint32). This is a DIFFERENT
 * variable from the WaitVSync counter above and both exist.
 *
 * It has no symbol of its own -- you cannot fingerprint a variable. Found via
 * tools/dataxref.py: the reference has exactly two accessors,
 * GetAndroidFrameCounter() (12 bytes, too short) and UnityPlayerLoop() (1060
 * bytes, matched 84.91% against a 0.75% runner-up). Mapping UnityPlayerLoop and
 * re-reading its adrp/ldr pair at the same offset gives the game's address:
 *   ref  0x9e0e20  adrp x8,#0x1880000 ; ldr w9,[x8,#0x460]  -> 0x1880460
 *   game 0x65b1c8  adrp x8,#0x11ad000 ; ldr w9,[x8,#0xae0]  -> 0x11adae0
 * It lands in .bss, adjacent to the Swappy globals page -- exactly where the
 * Android player's state lives.                                               */
#define OFF_UnityPlayerLoop           0x65b1ac
#define OFF_ANDROID_FRAME_COUNTER     0x11adae0   /* uint32_t                */

/* ---- Audio ----------------------------------------------------------------
 * AudioManager::InitNormal(bool, FMOD_OUTPUTTYPE), 2128 bytes, matched 92.48%
 * against a 0.38% runner-up. The output-type selection is byte-identical to the
 * reference, constants and all:
 *   +0xb8  mov  w8,#0x15        AUDIOTRACK(21)
 *   +0xbc  mov  w9,#0x17        AAUDIO(23)
 *   +0xc0  csel w8,w9,w8,eq
 *   +0xc8  mov  w9,#0x16        OPENSL(22)
 *   +0xcc  csel w21,w9,w8,eq
 *   +0xd4  mov  w1,w21          <- the argument to FMOD::System::setOutput
 *
 * Forcing OPENSL means rewriting +0xd4 to `movz w1,#22`. This is the same patch
 * PvZ Fusion applied, and it matters: the AAudio path goes out through
 * org/fmod/FMODAudioDevice in Java, which the fake JNI cannot service.        */
#define OFF_AudioManager_InitNormal   0x758c00
#define OFF_FMOD_OUTPUT_ARG           0x758cd4   /* mov w1,w21               */
#define DFU_FMOD_ARG_FROM             0x2A1503E1u /* mov w1, w21             */
#define DFU_FMOD_ARG_TO               0x528002C1u /* movz w1, #22 (OPENSL)   */

/* FMOD::OutputOpenSL::init(...), 1156 bytes, matched 95.85%. PvZ patched the
 * buffer geometry inside this function. Recorded for reference; no patch is
 * applied here, because the specific geometry PvZ needed was tuned to its own
 * mixer settings and should be re-derived on hardware if audio stutters.     */
#define OFF_FMOD_OutputOpenSL_init    0xdcf9e0

/* ---- Reference-inventory sites: located, not patched ----------------------
 * The PvZ Fusion and Bouncemasters notes between them name a set of engine
 * sites their ports needed. Everything applicable to this build was located so
 * that a future bring-up problem does not start from zero. NONE of these is
 * patched -- they are anchors. See AUDIT.md for the full coverage table and
 * for why each is or is not applicable here.                                 */
#define OFF_AudioManager_InitFMOD       0x758370   /* sub sp,sp,#0x190        */
#define OFF_GetAndroidAudioOutputType   0x65e39c   /* AndroidAudio::          */
#define OFF_UnityInitApplication        0x659270
#define OFF_ContextGLES_OnPostCreateSurface 0x641d5c
#define OFF_WindowContextEGL_Present    0x6bdbf8
#define OFF_ProcessTouchEvent           0x654024   /* android::view::MotionEvent */

/* ---- Garbage collection ---------------------------------------------------
 * There is NOTHING to patch on the libunity side. Searching the reference for
 * GarbageCollect / StopWorld / StartWorld / GCManager turns up only managed
 * bindings (GarbageCollector_CUSTOM_SetMode, _CollectIncremental, ...) and
 * container boilerplate -- thin wrappers over the IL2CPP collector.
 *
 * The real stop-the-world machinery is Boehm's, inside libil2cpp, and the four
 * addresses that matter are the GC_* group above: they were derived from the
 * only two pthread_kill and only two sem_post call sites in that binary, which
 * makes them unambiguous. That is the GC bridge; libc_shim.c drives it.       */

#endif /* DFU_OFFSETS_H */


/* ==========================================================================
 * UnityEngine.Input -- READ hooks (the game asks us, we never call it)
 * ==========================================================================
 *
 * This is the input route the zombotron_nx port actually ships, and it is
 * strictly better than poking TouchscreenInputManager from C.
 *
 * DIRECTION IS THE WHOLE POINT. Our old Tier 2 bridge CALLED INTO managed code
 * (SetKey/SetAxis/TriggerAction) from the render loop's C frame, where a managed
 * exception is an immediate std::terminate because a C frame cannot catch a C++
 * one. Hooking Input.GetKey inverts that: managed code calls OUT to us and we
 * return a bool. Nothing is invoked, so nothing can throw across the boundary.
 *
 * It is also the path Daggerfall's own InputManager reads, so every existing
 * keybinding, menu and remap screen works untouched -- the log already shows the
 * bindings resolving ("Jump->KeyCode 32, Run->KeyCode 304").
 *
 * NOT icall stubs. Unlike the UnityEngine.Time accessors (guard 0xA9BF4FFE,
 * "stp x30,x19,[sp,#-0x10]!"), these are real IL2CPP-compiled methods with
 * bodies -- Input.GetKey(KeyCode) calls the GetKeyInt icall internally. Their
 * prologue is "str x30,[sp,#-0x20]!" = 0xF81E0FFE, verified against the shipped
 * binary. RVAs from dump.cs, each confirmed to carry that prologue.
 *
 * NO TRAMPOLINE, DELIBERATELY. A hook that owns a keycode answers; for every
 * other keycode it returns 0. On a Switch that is not an approximation, it is
 * the truth: there is no keyboard, so nothing else can be pressing a key. That
 * removes the need to preserve and call the original body, which is the fiddly
 * and crash-prone half of hooking. */
#define OFF_Input_GetKey               0x3eac444
#define OFF_Input_GetKeyUp             0x3eac480
#define OFF_Input_GetKeyDown           0x3eac4bc
#define DFU_INPUT_GUARD_WORD           0xF81E0FFE   /* str x30,[sp,#-0x20]! */


/* ==========================================================================
 * Software keyboard: TextBox focus hook (dfu_keyboard.c)
 * ==========================================================================
 *
 * Daggerfall's TextBox does not use UnityEngine.TouchScreenKeyboard, so Unity
 * never drives the Java soft-input path and editbox.c's swkbd had nothing to
 * trigger it. TextBox::Update() shows where the real signal is:
 *
 *     ldrb w8, [x19, #0x3b0]        ; this->readOnly  -> if set, return
 *     bl   BaseScreenComponent::HasFocus()
 *     tbz  w0, #0, ...ret           ; no focus -> return
 *     ...
 *     bl   TextBox::HandleCharacterInput()
 *
 * HandleCharacterInput is therefore reached ONLY for a writable, focused text
 * box, and `this` is in x0. Being called is the focus signal.
 *
 * This is the one hook in the port that must let the original RUN -- it is what
 * actually types -- so its first four instructions are relocated into a
 * trampoline. They are PC-independent (sub sp + three stp), which is what makes
 * that legal; the guard below is those exact words, so a build whose prologue
 * starts with an adrp or a branch skips the hook instead of corrupting it. */
#define OFF_TextBox_HandleCharacterInput  0x1f9b6fc
#define OFF_TextBox_HandleControlInput    0x1f9b564   /* not hooked: opens adrp */
#define OFF_TextBox_Update                0x1f9b264
#define OFF_TextBox_set_Text              0x1f9a9fc
#define OFF_il2cpp_string_new             0x1bd7a48   /* exported symbol       */

/* sub sp,sp,#0x40 ; stp x30,x23,[sp,#0x10] ; stp x22,x21,[sp,#0x20] ;
 * stp x20,x19,[sp,#0x30]  -- read from the binary, never typed. */
#define DFU_TEXTBOX_HCI_PROLOGUE \
  { 0xD10103FFu, 0xA9015FFEu, 0xA90257F6u, 0xA9034FF4u }

/* DaggerfallUI instance fields, for the per-character route if set_Text ever
 * proves unsuitable (it is simpler, so it is what dfu_keyboard.c uses):
 *   char    lastCharacterTyped;  // +0xA8
 *   KeyCode lastKeyCode;         // +0xAC   */
#define DFU_UI_lastCharacterTyped   0xA8
#define DFU_UI_lastKeyCode          0xAC


/* ==========================================================================
 * BUILD IDENTITY -- which APK these offsets were derived from
 * ==========================================================================
 *
 * EVERY address in this header and in nx_patch_dfu.h was derived from one
 * specific build of Daggerfall Unity. Point them at a different APK and they
 * are not approximately right, they are meaningless.
 *
 * Most of the hooks guard themselves: a Time accessor, an Input method or a
 * TextBox prologue that does not match its expected first word is skipped with
 * a log line. THE 21 ALLOCATOR PATCH SITES IN libunity DO NOT. They are written
 * unconditionally, because they were located by matching a constellation of
 * instructions rather than by a single guard word -- so on the wrong build they
 * scribble 21 words into whatever happens to live at those addresses, and the
 * process dies early, somewhere unrelated, with nothing pointing back here.
 *
 * That is the failure a tester on the wrong APK gets, and it looks nothing like
 * a version-mismatch message. Hence the size check at load: cheap, needs no ELF
 * parsing, and two different builds are never the same number of bytes.
 *
 *   libunity.so   18,023,304 bytes   BuildID (xxHash) 1dc173bbf7a97f88
 *   libil2cpp.so  73,037,032 bytes   BuildID (sha1)   cc72326d...a77f0cda
 *
 * From: dfu_il2cpp-64bit-v1_1_1_9_mods-not-supported.apk
 * (Unity 2022.3.62f3, arm64-v8a, IL2CPP) */
#define EXPECT_LIBUNITY_BYTES   18023304u
#define EXPECT_LIBIL2CPP_BYTES  73037032u
#define EXPECT_APK_NAME         "dfu_il2cpp-64bit-v1_1_1_9"
