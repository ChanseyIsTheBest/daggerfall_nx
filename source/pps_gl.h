/* pps_gl.h -- the one EGL entry point the module imports. MIT licensed.
 *
 * libpapapearsaga.so imports exactly one egl* symbol: eglGetProcAddress. On
 * Android, GLSurfaceView owned the context and did the swap, so the engine
 * never needed the rest of EGL -- main.c owns it here and the game never sees
 * it. The GL side is the full GLES2 set (142 functions) and nothing from
 * GLES3, so those bind straight to switch-mesa with no wrapper.
 */
#ifndef PPS_GL_H
#define PPS_GL_H

void *pps_egl_get_proc_address(const char *name);

/* GL_EXT_discard_framebuffer. The engine asks for this by name through
 * eglGetProcAddress and calls it after resolving its offscreen pass. See the
 * .c file for why it is a no-op rather than forwarded. */
void pps_glDiscardFramebufferEXT(unsigned target, int numAttachments,
                                 const unsigned *attachments);

#endif
