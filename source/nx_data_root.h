/* nx_data_root.h -- resolve the game folder at RUNTIME from wherever the .nro
 * and the game files actually live, instead of hardcoding a folder name.
 *
 * Adopted from clonehero_nx, widened to scan the whole SD card.
 *
 * WHY. Every path in this loader used to be built from a compile-time
 * `sdmc:/switch/<GAME_FOLDER>`, with GAME_FOLDER == "daggerfall". Stage the
 * card as `switch/daggerfall_nx/` -- which is the obvious name, and the one
 * this port's own .nro is called -- and you get "could not find libmain.so"
 * about a file plainly visible on the card. Worse, debug.log lives under that
 * same root, so a wrong root also means NO LOG to diagnose from: the port
 * fails silently and tells you nothing.
 *
 * clonehero_nx lost a boot to exactly this (`switch/clonehero_nx` vs
 * `switch/clonehero`). This port then lost one too, the same way. The folder
 * name should never have been load-bearing.
 *
 * RESOLUTION ORDER, first validated hit wins. Every candidate is checked by
 * stat()ing libmain.so inside it, so a plausible-but-wrong path is rejected
 * rather than silently adopted:
 *
 *   1. argv[0]'s directory     hbmenu passes the full .nro path, so the folder
 *                              name is irrelevant when launched that way.
 *   2. compile-time default    sdmc:/switch/<GAME_FOLDER>
 *   3. scan sdmc:/switch/      one level
 *   4. scan sdmc:/             one level -- "any folder on the card"
 *   5. scan sdmc:/<dir>/       two levels, for cards organised as
 *                              e.g. sdmc:/games/daggerfall/
 *   6. give up -> the compile-time default, so the on-screen error names a
 *                 sensible path rather than an empty string.
 *
 * Steps 3-5 matter more here than they did for Clone Hero. This port is meant
 * to be launched by TITLE OVERRIDE (hold R), and a title-override launch does
 * not necessarily hand over a useful argv[0] -- so the scan is the primary
 * mechanism in normal use, not a fallback.
 */
#ifndef NX_DATA_ROOT_H
#define NX_DATA_ROOT_H

/* Resolved once, very early in main(), BEFORE the first debugPrintf. */
extern char g_data_root[512];   /* e.g. "sdmc:/switch/daggerfall_nx"        */
extern char g_log_path[576];    /* g_data_root + "/debug.log"               */

/* How the root was found -- logged once so a bad layout is self-diagnosing. */
extern char g_data_root_how[512];

/* Must be the first thing main() calls. Safe with argc==0 / argv==NULL. */
void nx_resolve_data_root(int argc, char *argv[]);

/* Build "<g_data_root><sub>" into a rotating static buffer. `sub` must start
 * with '/' (or be ""). Replaces the old `DATA_ROOT "/thing"` compile-time
 * concatenation, which cannot work once the root is a runtime value.
 *
 * ROTATING BUFFERS: 8 slots, so a couple of calls can be live in one
 * expression. Do not stash the returned pointer long-term -- copy it. */
const char *nx_path(const char *sub);

/* Does <root>/arena2 exist? Daggerfall needs the original game data, and a
 * missing or misplaced ARENA2 is the second thing that goes wrong after the
 * root itself. Logged at boot so it is answered before it is asked. */
int nx_have_arena2(void);

#endif /* NX_DATA_ROOT_H */
