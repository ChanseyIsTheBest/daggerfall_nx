/* nx_exception_dump.c -- user exception handler: on any fault, dump symbolized
 * PC/LR + all GPRs + the faulting thread's stack frame + (for the UIGeometryJob
 * crash) the job data and element record, straight into debug.log. Then break so
 * Atmosphere's creport still fires. Every read is svcQueryMemory-guarded.
 *
 * Why: the recurring crash at libunity+0x871354 (UI::UIGeometryJob, str s0,[x8])
 * has a garbage destination pointer whose producer static analysis can't pin
 * down (two-mode 7KB function, partial [sp] slot visibility). This dumps the
 * ACTUAL frame: [sp+0x150] (the csel source), [sp+0x100/0x160/0x168] (channel
 * table), [sp+0x48/0x50] (element base + index), [sp+0x10] (out base),
 * [sp+0x28] (job data) -- ground truth in one crash.
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "diag.h"   /* diag_resume_all_gc_paused */
#include "jni_fake.h"
#include "so_util.h"

/* libnx user exception handling: providing these symbols + the handler enables it */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static int xd_readable(uintptr_t addr, size_t len) {
  if (!addr || addr < 0x1000) return 0;
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == MemType_Unmapped) return 0;
    if ((mi.perm & Perm_R) == 0) return 0;
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

/* "libunity.so+0x871354" style annotation; falls back to raw hex */
static const char *xd_sym(u64 v, char *buf, size_t n) {
  so_module *m = so_find_module_by_addr((const void *)v);
  if (m)
    snprintf(buf, n, "%s+0x%lx", m->name, (unsigned long)(v - (uintptr_t)m->load_virtbase));
  else
    snprintf(buf, n, "%016lx", (unsigned long)v);
  return buf;
}

static void xd_dump_range(const char *tag, uintptr_t base, size_t bytes) {
  if (!xd_readable(base, bytes)) {
    debugPrintf("[xd] %s @%p: UNREADABLE\n", tag, (void *)base);
    return;
  }
  char s[64];
  for (size_t off = 0; off < bytes; off += 0x20) {
    const u64 *q = (const u64 *)(base + off);
    debugPrintf("[xd] %s+%03zx: %016lx %016lx %016lx %016lx\n",
                tag, off, (unsigned long)q[0], (unsigned long)q[1],
                (unsigned long)q[2], (unsigned long)q[3]);
    (void)s;
  }
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  /* Reprint the JNI approximation ledger into the fault log. A crash report
   * that already carries "here is everything we faked, and here is what Unity
   * read back" saves a whole second run. Declared in jni_fake.h; a no-op when
   * DFU_JNI_LOUD is 0. */
  jni_approx_summary("crash");
  char b1[96], b2[96], b3[96];
  /* If a fault lands while the collector has threads suspended for a
   * stop-the-world, nothing will ever resume them and the console wedges --
   * a worse outcome than the crash itself, and it would hide the crash too.
   * Release them before doing anything else. Safe when none are paused. */
  diag_resume_all_gc_paused();
  { extern void debug_log_flush(void); debug_log_flush(); }   /* whatever led here */
  { extern void gc_paused_live_reset(void); gc_paused_live_reset(); }

  debugPrintf("[xd] ================= USER EXCEPTION =================\n");
  debugPrintf("[xd] desc=0x%x pc=%s far=%016lx esr=%08x\n",
              ctx->error_desc, xd_sym(ctx->pc.x, b1, sizeof b1),
              (unsigned long)ctx->far.x, ctx->esr);
  debugPrintf("[xd] lr=%s sp=%016lx fp=%016lx\n",
              xd_sym(ctx->lr.x, b2, sizeof b2),
              (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
  for (int i = 0; i < 28; i += 4)
    debugPrintf("[xd] x%-2d %016lx  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx\n",
                i, (unsigned long)ctx->cpu_gprs[i].x,
                i + 1, (unsigned long)ctx->cpu_gprs[i + 1].x,
                i + 2, (unsigned long)ctx->cpu_gprs[i + 2].x,
                i + 3, (unsigned long)ctx->cpu_gprs[i + 3].x);
  debugPrintf("[xd] x28 %016lx\n", (unsigned long)ctx->cpu_gprs[28].x);

  /* ---- the recurring vtable-dispatch fault -------------------------------
   * Seen three times now, always identically:
   *   esr = 8a000000 (EC 0x22, PC alignment fault), pc == x5,
   *   lr  = libil2cpp.so+0x1db3744
   * and pc always equals (some valid pointer >> 8) with a stray byte in the
   * top -- verified against all three samples.
   *
   * The faulting site is libil2cpp+0x1db617c:
   *     ldr x9, [x0]              ; x9 = the object's first word (its class)
   *     ldp x5, x4, [x9, #0x178]  ; {method, MethodInfo} out of it
   *     br  x5
   * Horizon permits unaligned loads on normal memory, so a misaligned x9 does
   * not fault at the ldp -- it quietly returns the bytes shifted by one, and
   * the branch takes the blame several instructions later. x9 has ended in
   * 0xe34531 in every run, on three different ASLR bases: deterministic, not
   * random corruption.
   *
   * What is still unknown is WHICH object x0 is. So dump it. The next
   * occurrence answers the question instead of being a fourth sample. */
  {
    const uint64_t esr = ctx->error_desc, pc = ctx->pc.x, x5 = ctx->cpu_gprs[5].x;
    if (pc == x5 && (pc & 3)) {
      const uint64_t x0 = ctx->cpu_gprs[0].x, x9 = ctx->cpu_gprs[9].x;
      debugPrintf("[xd] --- vtable-dispatch fault (see AUDIT sec 23) ---\n");
      debugPrintf("[xd]   implied good ptr = %016lx  (pc<<8, stray top byte 0x%02lx)\n",
                  (unsigned long)((pc & 0xFFFFFFFFul) << 8), (unsigned long)(pc >> 56));
      debugPrintf("[xd]   x0 (object) = %016lx   x9 (*obj, should be a class) = %016lx %s\n",
                  (unsigned long)x0, (unsigned long)x9, (x9 & 7) ? "MISALIGNED" : "aligned");
      /* First 8 words of the object: a real managed object starts with a class
       * pointer then a monitor pointer. A fake JNI object from this port starts
       * with one of our TAG_* magic words, which is the thing to look for. */
      /* xd_readable() first. This runs INSIDE the exception handler, so a
       * nested fault here would destroy the dump it is trying to produce --
       * and x0 is by definition a pointer we already distrust. */
      if (x0 && !(x0 & 7) && xd_readable((uintptr_t)x0, 64)) {
        const uint64_t *o = (const uint64_t *)x0;
        for (int i = 0; i < 8; i += 4)
          debugPrintf("[xd]   obj+%02x: %016lx %016lx %016lx %016lx\n", i * 8,
                      (unsigned long)o[i], (unsigned long)o[i+1],
                      (unsigned long)o[i+2], (unsigned long)o[i+3]);
        /* Name the tag if obj[0] IS one of ours -- that would mean a fake JNI
         * object reached managed dispatch, which is the standing hypothesis
         * this dump exists to test. Byte-for-byte comparison against the
         * observed values has already RULED OUT the tags being x9 itself
         * (see AUDIT sec 25), but obj[0] is a different word and untested. */
        {
          const unsigned long t = (unsigned long)(o[0] & 0xFFFFFFFFul);
          const char *nm = t == 0x4f424a31ul ? "TAG_OBJECT 'OBJ1'"
                         : t == 0x53545231ul ? "TAG_STRING 'STR1'"
                         : t == 0x4f415231ul ? "TAG_OBJARR 'OAR1'"
                         : t == 0x50415231ul ? "TAG_PRIARR 'PAR1'"
                         : t == 0x4d494431ul ? "TAG_ID 'MID1'"
                         : t == 0x434c5331ul ? "TAG_CLASS 'CLS1'"
                         : t == 0x424d5031ul ? "BITMAP_TAG 'BMP1'" : NULL;
          debugPrintf("[xd]   obj[0] low32 = %08lx %s\n", t,
                      nm ? nm : "(not one of this port's JNI tags)");
          if (nm)
            debugPrintf("[xd]   >>> A FAKE JNI OBJECT REACHED MANAGED DISPATCH.\n");
        }
      }
      /* And the class word itself, if it points anywhere readable. */
      if (x9 && xd_readable((uintptr_t)(x9 & ~7ull), 32)) {
        const uint64_t *k = (const uint64_t *)(uintptr_t)(x9 & ~7ull);
        debugPrintf("[xd]   *(x9 & ~7) = %016lx %016lx %016lx %016lx\n",
                    (unsigned long)k[0], (unsigned long)k[1],
                    (unsigned long)k[2], (unsigned long)k[3]);
      }
      (void)esr;
    }
  }

  /* frame-pointer backtrace (same walk as the watchdog) */
  uintptr_t fp = (uintptr_t)ctx->fp.x;
  for (int d = 0; d < 12 && fp; d++) {
    if (!xd_readable(fp, 16)) break;
    uintptr_t nfp = ((uintptr_t *)fp)[0];
    uintptr_t rlr = ((uintptr_t *)fp)[1];
    if (!rlr) break;
    debugPrintf("[xd]   bt[%d] %s\n", d, xd_sym(rlr, b3, sizeof b3));
    if (nfp <= fp) break;
    fp = nfp;
  }

  /* the whole stack frame: every [sp+slot] the crash block reads */
  uintptr_t sp = (uintptr_t)ctx->sp.x;
  xd_dump_range("SP", sp, 0x200);

  /* UIGeometryJob specifics (harmless if this is a different crash):
   * [sp+0x48]=element base, [sp+0x50]=element index (stride 0x70),
   * [sp+0x28]=job data, [sp+0x10]=output base. */
  if (xd_readable(sp + 0x58, 8)) {
    uintptr_t elem_base = ((uintptr_t *)(sp + 0x48))[0];
    uintptr_t elem_idx  = ((uintptr_t *)(sp + 0x50))[0];
    uintptr_t jobdata   = ((uintptr_t *)(sp + 0x28))[0];
    uintptr_t outbase   = ((uintptr_t *)(sp + 0x10))[0];
    debugPrintf("[xd] elem_base=%016lx idx=%lu jobdata=%016lx outbase=%016lx\n",
                (unsigned long)elem_base, (unsigned long)elem_idx,
                (unsigned long)jobdata, (unsigned long)outbase);
    if (elem_idx < 0x10000 && elem_base)
      xd_dump_range("ELEM", elem_base + elem_idx * 0x70, 0x70);
    if (jobdata) xd_dump_range("JOB", jobdata, 0x60);
  }
  debugPrintf("[xd] ============== END EXCEPTION DUMP ==============\n");
  { extern void debug_log_flush(void); debug_log_flush(); }   /* and the dump itself */

  /* re-raise so the process still aborts and Atmosphere writes its report */
  svcBreak(BreakReason_Panic, 0, 0);
  for (;;) svcSleepThread(1000000000ULL);
}
