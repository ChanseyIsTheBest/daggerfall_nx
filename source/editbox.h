/* editbox.h -- Switch software-keyboard backing for the engine's EditBox/TextBox
 *
 * The engine drives name/text entry through JNI ShowEditBox / IsOpenEditBox /
 * GetEditBoxText (it renders its own field and polls the text). We back this
 * with libnx swkbd: ShowEditBox pops the Switch keyboard (blocking), and the
 * result is read back through GetEditBoxText once IsOpenEditBox reports closed.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __EDITBOX_H__
#define __EDITBOX_H__

// show the software keyboard (blocking) seeded with `initial`, limited to
// `maxlen` characters; stores the result for editbox_text().
void editbox_show(const char *initial, int maxlen);

// 1 while the keyboard is up, 0 once the user has confirmed/cancelled.
int editbox_is_open(void);

// the last entered text (stable pointer; the engine copies it out).
const char *editbox_text(void);

/* 1 if the last editbox_show() was dismissed without confirming. editbox_text()
 * returns the unchanged seed in that case, which is indistinguishable from the
 * user retyping the same thing -- so check this before writing anything back. */
int editbox_cancelled(void);

/* Unity's own showSoftInput path drives the keyboard on this game; when it does,
 * the TextBox auto-open in dfu_keyboard.c gets out of the way. */
void editbox_mark_engine_driven(void);
int  editbox_engine_drives(void);

// engine asked to dismiss the box.
void editbox_close(void);

#endif
