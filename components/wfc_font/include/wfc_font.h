/* The font the panel is drawn in.
 *
 * LVGL ships a Source Han Sans SC build, and P2 through P4 used it. It does
 * not work for this: it is a fixed ~1100 character subset chosen for LVGL's
 * own demos, leaning traditional and Japanese, and a character outside it
 * draws as nothing at all -- no box, no question mark, a gap. The P4 panel
 * was rendering 会话 as 会, 状态 as 状 and 连接 as 接, correctly, silently,
 * with the serial log showing the right string the whole time.
 *
 * That is survivable for a status panel and not survivable for a chat client,
 * where the text on screen is whatever somebody typed on their phone. So this
 * is a font generated for the job (tools/make_font.sh):
 *
 *   ASCII, GB2312 level 1 (3755 characters -- effectively all modern
 *   simplified Chinese), CJK punctuation, a few geometric marks, and the
 *   LV_SYMBOL_* codepoints the keyboard widget builds its key map from.
 *
 * 3800 glyphs at 16 px and 4 bpp, about 480 KB of flash. Noto Sans SC, under
 * the SIL Open Font License 1.1 (LICENSE-OFL.txt).
 *
 * One size, deliberately: a second size is another 480 KB for something a
 * colour or a weight can say instead. Where small Latin text is genuinely
 * better -- timestamps, counters -- use lv_font_montserrat_12, which is built
 * into LVGL and costs nothing extra.
 *
 * Still not covered: GB2312 level 2 (rare characters, mostly in names),
 * traditional Chinese, kana, emoji. Those still draw as gaps, which is why
 * tools/check_font.py exists -- run it over the sources and it names every
 * literal the panel cannot draw.
 */

#ifndef WFC_FONT_H
#define WFC_FONT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t wfc_font_16;

#ifdef __cplusplus
}
#endif

#endif /* WFC_FONT_H */
