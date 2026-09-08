/* pps_log.h -- __android_log_* mirrored into debug.log. MIT licensed.
 * See pps_log.c for why this is worth having on this engine in particular. */
#ifndef PPS_LOG_H
#define PPS_LOG_H

int  pps_log_print(int prio, const char *tag, const char *fmt, ...)
     __attribute__((format(printf, 3, 4)));
int  pps_log_write_ndk(int prio, const char *tag, const char *text);
void pps_set_abort_message(const char *msg);

/* stdout, redirected.
 *
 * The module's printf/puts would otherwise go to the console devoptab, which
 * on a Switch homebrew is either nothing at all or the framebuffer that EGL
 * now owns -- and writing to the latter while a GL context is current is a
 * good way to lose the display. The engine prints on a few error paths, so
 * these route it to debug.log where it can actually be read. */
int  pps_printf_discard(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  pps_vprintf_discard(const char *fmt, __builtin_va_list ap);
int  pps_puts_discard(const char *s);

#endif
