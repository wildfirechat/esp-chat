/* The IME's dictionary. Generated -- see tools/make_pinyin.py.
 *
 * LVGL's pinyin IME comes with a dictionary of its own and it is the wrong
 * one: 321 syllables of traditional Chinese, chosen to match the characters
 * in LVGL's demo font. On it, "xie" offers 謝 and "ti" offers 題.
 *
 * This one is built from GB2312 level 1 -- the same 3755 characters
 * components/wfc_font/ can draw -- with candidates ordered by how common the
 * character is. That ordering is the difference between an input method and a
 * lookup table: "wo" has to put 我 first.
 *
 * Set CONFIG_LV_IME_PINYIN_USE_DEFAULT_DICT=n so LVGL's own does not come
 * along for the ride; it is 24 KB of flash for a language this board does not
 * write in.
 */

#ifndef UI_PINYIN_DICT_H
#define UI_PINYIN_DICT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NULL-terminated, sorted by pinyin. lv_ime_pinyin_set_dict() takes a
 * non-const pointer but only ever reads, so this stays in flash. */
extern const lv_pinyin_dict_t ui_pinyin_dict[];

#ifdef __cplusplus
}
#endif

#endif /* UI_PINYIN_DICT_H */
