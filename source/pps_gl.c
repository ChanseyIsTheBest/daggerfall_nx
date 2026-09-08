/* pps_gl.c -- eglGetProcAddress. MIT licensed. See pps_gl.h. */

#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "pps_gl.h"

/* GL_EXT_discard_framebuffer is a HINT: it tells a tiler that the contents of
 * an attachment need not be written back to memory. Honouring it is an
 * optimisation and ignoring it is always correct.
 *
 * A no-op rather than a forward to mesa's entry point, for two reasons. The
 * nouveau driver behind switch-mesa is not a tiler, so there is nothing to
 * save. And the engine calls this immediately after unbinding its offscreen
 * FBO but before sampling the texture it just rendered into -- a driver that
 * took the hint literally at the wrong moment would discard live contents.
 * Silently doing nothing is the safe reading of a hint. */
void pps_glDiscardFramebufferEXT(unsigned target, int numAttachments,
                                 const unsigned *attachments) {
  (void)target; (void)numAttachments; (void)attachments;
}

/* Scanning the binary's .rodata for glGetProcAddress-shaped strings turns up
 * exactly one extension name, so this table is complete rather than a
 * starting point. It is still a table: an engine build that asked for a
 * second extension would otherwise get a null pointer it does not check,
 * and the log line below is what would tell you.
 *
 * Note that the entry points must NOT be resolved by falling through to
 * mesa's own eglGetProcAddress. mesa would happily return its address for a
 * core GLES2 function too, and the module already has those bound directly
 * through the import table -- two routes to the same function that could
 * disagree after a context change is a bug waiting to happen. */
typedef struct { const char *name; void *fn; } GlExt;

static const GlExt g_ext[] = {
  { "glDiscardFramebufferEXT", (void *)pps_glDiscardFramebufferEXT },
};

void *pps_egl_get_proc_address(const char *name) {
  unsigned i;
  if (!name) return NULL;
  for (i = 0; i < sizeof(g_ext) / sizeof(*g_ext); i++)
    if (!strcmp(name, g_ext[i].name))
      return g_ext[i].fn;

  /* Not an error. The engine tests the result and falls back to the
   * unextended path, which is what we want for anything not listed. Logged
   * because a new name appearing here is worth knowing about. */
  LOGB("eglGetProcAddress(\"%s\") -> not provided", name);
  return NULL;
}
