/* pps_input.h -- turning the console's input into onTouchEvent calls.
 * MIT licensed. See pps_input.c.
 */
#ifndef PPS_INPUT_H
#define PPS_INPUT_H

/* The engine's touch entry point, bound in main.c:
 *     onTouchEvent(JNIEnv*, jobject, jint id, jint type, jint x, jint y)
 * Argument order confirmed against GameView$5.run's iget sequence in the dex,
 * which reads val$id before val$finalType. Getting id and type the wrong way
 * round does not crash -- it delivers pointer 2 ("move") as a down event on
 * pointer 0, so the first tap works and nothing after it does. */
typedef void (*pps_touch_fn)(void *env, void *thiz, int id, int type, int x, int y);

void pps_input_init(pps_touch_fn touch, void *env, void *thiz);

/* Poll the pad, touchscreen and mouse and emit whatever changed. Call once per
 * frame, before step(). */
void pps_input_poll(void);

/* Edge-triggered Android Back, read from a PadState of its own. */
int  pps_input_back_pressed(void);

/* Hold + and - together for ~1.5 s. The game has no quit of its own. */
int  pps_input_quit_requested(void);

/* Cancel every live pointer. Called on focus loss, so the engine does not
 * resume mid-drag with a finger it thinks is still down. */
void pps_input_cancel_all(void);

#endif
