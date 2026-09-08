/* imports.h -- the resolver table. MIT licensed.
 *
 * libc_shim.c is compiled in unmodified and includes this by name for
 * dynlib_find_export, which is what backs its dlsym() shim. That matters more
 * here than in the reference ports: this engine dlopen()s libOpenSLES.so and
 * resolves its entire audio interface through dlsym, so this lookup is on the
 * path to any sound at all.
 */
#ifndef PPS_IMPORTS_H
#define PPS_IMPORTS_H

#include <stddef.h>
#include <stdint.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern const size_t   dynlib_numfunctions;

uintptr_t dynlib_find_export(const char *name);

#endif
