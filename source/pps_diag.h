/* pps_diag.h -- the watchdog hooks imports_helpers.c calls.
 *
 * MIT licensed. See LICENSE.
 *
 * imports_helpers.c is reused from the Osmos port, and its pthread shims
 * announce every blocking wait to a watchdog so that a hang can be attributed
 * to a specific mutex, condvar or join rather than to "it stopped".
 *
 * That watchdog is not carried over here. It pauses threads to read their
 * registers, which is expensive enough to change the timing of the thing it is
 * measuring, and this port has 240 static initialisers and several engine
 * worker threads to get through before it would tell you anything.
 *
 * So these compile to nothing by default and imports_helpers.c drops in
 * unmodified. Set PPS_DIAG to 1 to have the waits logged instead, which is
 * enough to identify a deadlock's participants without the register-reading
 * machinery.
 */
#ifndef PPS_DIAG_H
#define PPS_DIAG_H

#include "config.h"

enum { DIAG_W_MUTEX = 1, DIAG_W_COND, DIAG_W_JOIN, DIAG_W_SEM };

#if PPS_DIAG
void diag_wait_enter(int kind, const void *object);
void diag_wait_exit(void);
void diag_thread_register(void *entry, int id);
void diag_thread_unregister(void);
#else
#define diag_wait_enter(kind, obj)      ((void)(obj))
#define diag_wait_exit()                ((void)0)
#define diag_thread_register(entry, id) ((void)(entry))
#define diag_thread_unregister()        ((void)0)
#endif

#endif
