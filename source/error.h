/* error.h -- error handler
 *
 * Copyright (C) 2021 fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ERROR_H__
#define __ERROR_H__

void startup_status_begin(const char *message);
void startup_status_update(const char *message);
void startup_status_end(void);
void fatal_error(const char *fmt, ...)
    __attribute__((noreturn)) __attribute__((format(printf, 1, 2)));

/* Non-fatal: show a readable message and return once + is pressed. Used for
 * the missing-files case, which is the most common first-run failure. */
void error_screen(const char *msg);

/* A console and an EGL surface cannot both own the default window. main.c
 * installs egl_stop() here so the error paths can reclaim the screen without
 * this file needing to know about EGL. */
void error_set_gfx_release(void (*fn)(void));

#endif
