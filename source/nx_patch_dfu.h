/* nx_patch_dfu.h -- in-memory libunity.so patch table for DAGGERFALL UNITY
 * (Unity 2022.3.62f3, arm64, IL2CPP).
 *
 * Target binary these offsets were derived from:
 *     libunity.so   BuildID xxHash 1dc173bbf7a97f88   18,023,304 bytes
 * If your libunity.so has a different BuildID, EVERY offset in this file is
 * wrong. Run `python3 tools/verify_offsets.py <your libunity.so>` first; it
 * re-derives the whole table from the binary and tells you.
 *
 * WHAT IT DOES
 *   Unity's block allocator reserves memory in 256MB-aligned regions. On a 4GB
 *   Switch that granularity does not fit the so_loader address space, so we
 *   rewrite the allocator's region-size computation to use 64MB granularity.
 *   Each entry rewrites one 32-bit instruction word: {from} is the stock word,
 *   {to} is the 64MB-granularity word (shift 0x1c->0x1a, mask/const 256MB->64MB).
 *   The transforms are IDENTICAL to Zookeeper DX (62f2) and PvZ Fusion (62f1c1);
 *   only the addresses differ. That is expected -- same 2022.3.62 codegen.
 *
 * HOW THESE OFFSETS WERE DERIVED
 *   Constellation matching against the PvZ Fusion 3.8.1 table. The 21 words fall
 *   into 4 clusters (4 distinct allocator functions). For each cluster we
 *   searched this binary for the cluster's first {from} word and required every
 *   other word in the cluster to match at its exact reference-relative delta.
 *
 *   NOTE ON ADDRESSES: the search operates on file offsets; the table below
 *   stores LINK-TIME VADDRS, which is what so_patch_code() needs (runtime addr
 *   = load_virtbase + off). libunity's executable segment maps file 0x37cec0 to
 *   vaddr 0x380ec0, so every entry here is its file offset + 0x4000. Getting
 *   this backwards is a silent 16 KB miss; tools/verify_offsets.py catches it.
 *
 *   Result: each of the 4 clusters matched at EXACTLY ONE address, and all four
 *   landed at the SAME delta from the PvZ table: -0x286e4. Four independent
 *   functions agreeing on one shift is not something a false positive produces.
 *   Confidence: VERY HIGH.
 *
 *   Confirmed to live in TLSAllocator<0>::ThreadInitialize,
 *   DynamicHeapAllocator::DynamicHeapAllocator,
 *   MemoryManager::GetAllocatorContainingPtr and
 *   MemoryManager::VirtualAllocator::GetMemoryBlockFromPointer -- the same four
 *   the PvZ notes name.
 *
 * SAFETY
 *   nx_patch_libunity() is VERIFY-FIRST: it reads each target word and only
 *   patches if it already equals {from}; if ANY site mismatches it patches
 *   NOTHING and logs loudly. A wrong offset here is caught, not catastrophic.
 */
#ifndef NX_PATCH_DFU_H
#define NX_PATCH_DFU_H

#include <stdint.h>

/* ---- Phase flags ----------------------------------------------------------
 * Engine-internal hooks whose offsets are NOT yet derived for this build. Both
 * OFF for first bring-up. See PORTING.md "Offsets still needed".            */
/* All three are now DERIVED (2022.3.62f3 version-matched symbol pair) and ON.
 * Addresses and their derivation live in dfu_offsets.h. Each site is
 * guard-checked before it is written, so a binary mismatch disables the hook
 * loudly rather than corrupting .text. */
#define DFU_HAVE_TIME_FIX      1  /* TimeManager::Update hook @0x4bfe14, GetTimeManager @0x4c0448 */
#define DFU_HAVE_FMOD_FORCE    1  /* force FMOD_OUTPUTTYPE_OPENSL @0x758cd4 */
#define DFU_HAVE_SWAPPY_FORCE  1  /* force Swappy::IsEnabledAndActive @0x649b0c -> 0 */
#define DFU_HAVE_FINISH_PROBE  0  /* PvZ-only il2cpp diagnostic; no meaning here */

typedef struct { uint32_t off, from, to; } NxPatchWord;

/* ---- 21 region-granularity sites (256MB -> 64MB) --------------------------
 *  All four clusters resolved uniquely at a uniform -0x2c6e4 from the PvZ
 *  3.8.1 table. The pvz address is kept in the comment so a future migration
 *  can re-run the same constellation search.                                */
static const NxPatchWord DFU_PATCH_WORDS[] = {
  /*  0 (pvz 0x419694) */ { 0x3f0fb0, 0x12be0009, 0x12bf8009 },
  /*  1 (pvz 0x41969c) */ { 0x3f0fb8, 0x92648d36, 0x92669536 },
  /*  2 (pvz 0x419f3c) */ { 0x3f1858, 0x52a20009, 0x52a08009 }, /* region-round: lsr#0x1c then mov#256MB */
  /*  3 (pvz 0x41dc64) */ { 0x3f5580, 0xd35cfd29, 0xd35afd29 },
  /*  4 (pvz 0x41dc68) */ { 0x3f5584, 0x52a2000a, 0x52a0800a },
  /*  5 (pvz 0x41e110) */ { 0x3f5a2c, 0x12be000a, 0x12bf800a },
  /*  6 (pvz 0x41e118) */ { 0x3f5a34, 0x92648d36, 0x92669536 },
  /*  7 (pvz 0x420118) */ { 0x3f7a34, 0xd35cdc33, 0xd35ad433 },
  /*  8 (pvz 0x42011c) */ { 0x3f7a38, 0xd35cfd15, 0xd35afd15 },
  /*  9 (pvz 0x4201ac) */ { 0x3f7ac8, 0x52a20008, 0x52a08008 }, /* passes 256MB as call arg */
  /* 10 (pvz 0x4204e8) */ { 0x3f7e04, 0xd35cfc28, 0xd35afc28 },
  /* 11 (pvz 0x4204f8) */ { 0x3f7e14, 0x92646c28, 0x92667428 },
  /* 12 (pvz 0x420500) */ { 0x3f7e1c, 0xd35c9c2a, 0xd35a942a },
  /* 13 (pvz 0x420514) */ { 0x3f7e30, 0xb25c6feb, 0xb25e77eb },
  /* 14 (pvz 0x420518) */ { 0x3f7e34, 0xd35cdc29, 0xd35ad429 },
  /* 15 (pvz 0x42051c) */ { 0x3f7e38, 0xf2a2000b, 0xf2a0800b },
  /* 16 (pvz 0x42055c) */ { 0x3f7e78, 0xcb0a7108, 0xcb0a6908 },
  /* 17 (pvz 0x420574) */ { 0x3f7e90, 0xd368fc28, 0xd366fc28 },
  /* 18 (pvz 0x420584) */ { 0x3f7ea0, 0xd35c9c29, 0xd35a9429 },
  /* 19 (pvz 0x422238) */ { 0x3f9b54, 0xd368fc28, 0xd366fc28 },
  /* 20 (pvz 0x422250) */ { 0x3f9b6c, 0xd35c9e89, 0xd35a9689 },
};
#define DFU_PATCH_WORDS_N ((int)(sizeof(DFU_PATCH_WORDS)/sizeof(DFU_PATCH_WORDS[0])))

/* ---- branch/word force sites ---------------------------------------------- */
#define DFU_HAVE_BRANCH_FORCES 1
static const NxPatchWord DFU_BRANCH_FORCES[] = {
  /* BufferGLES::BeginWrite caps gate.
   *
   * On Switch mesa/nouveau GL, the buffer MAP path (caps+0x57d != 0 ->
   * DataBufferGLES map -> glMapBufferRange) hands back a pointer whose high
   * half is stale; UIGeometryJob then writes through it and takes a data abort.
   * PvZ Fusion hit this five times at the same PC before forcing the CPU
   * staging path instead, which is the fully supported no-map-caps fallback:
   * EndWrite reads the stored 0 mode byte at buf+0x80 and uploads via
   * glBufferSubData.
   *
   * DERIVATION (this binary, not inherited): searching for the caps read
   * `ldrb w8,[x0,#0x57d]` (0x3955f408) gives 3 hits. Only 0x9db414 is followed
   * by `strb w8,[x20,#0x80]` -- BeginWrite's signature, and the same
   * entry+0x2c position the PvZ site sits at (fn entry here = 0x9db3e8).
   * The other two hits (file 0x886120, 0x9cdaf4) are a caps predicate returning
   * cset w0,ne and an unrelated 3-arg function.
   *
   * Daggerfall renders far more UI geometry than PvZ, so if anything this is
   * more load-bearing here. Set DFU_HAVE_BRANCH_FORCES to 0 to A/B it. */
  { 0x9df414, 0x3955f408, 0x52800008 },   /* ldrb w8,[x0,#0x57d] -> movz w8,#0 */

  /* Swappy::IsEnabledAndActive -> return 0.
   * We drive our own render loop, so Google's frame pacer must be inert; if it
   * stays live it fights our presentation. Symbol-derived: the reference's
   * _ZN6Swappy18IsEnabledAndActiveEv matched 24/24 instruction shapes here.
   * Rewrite the entry to `mov w0,#0 ; ret`. The stock first word is the
   * prologue `stp x30,x19,[sp,#-0x10]!`, which the verify-first check reads. */
  { 0x649b0c, 0xa9bf4ffe, 0x52800000 },   /* stp x30,x19,.. -> movz w0,#0      */
  { 0x649b10, 0xf0005b13, 0xd65f03c0 },   /* adrp x19,..    -> ret             */

  /* FMOD output type -> FMOD_OUTPUTTYPE_OPENSL (22).
   * Stock code cselects AUDIOTRACK(21) / AAUDIO(23) / OPENSL(22) and passes the
   * result in w1 to FMOD::System::setOutput. The AAudio path exits through
   * org/fmod/FMODAudioDevice in Java, which the fake JNI cannot service, so
   * pin the argument. Byte-identical to the reference at this site. */
  { 0x758cd4, 0x2a1503e1, 0x528002c1 },   /* mov w1,w21     -> movz w1,#22     */
};
#define DFU_BRANCH_FORCES_N (DFU_HAVE_BRANCH_FORCES ? \
  ((int)(sizeof(DFU_BRANCH_FORCES)/sizeof(DFU_BRANCH_FORCES[0]))) : 0)

#endif /* NX_PATCH_DFU_H */
