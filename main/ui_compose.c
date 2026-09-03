/* Writing a message.
 *
 * Its own page because there is no room for a keyboard anywhere else: the
 * content area is 212 px tall and a usable keyboard wants 130 of them. The
 * chat page therefore shows bubbles and one wide button, and typing happens
 * here, on top of it.
 *
 * The page collects text and hands it back through a callback. It is told a
 * title to show and nothing else -- not the conversation, not the client.
 * That is on purpose: recording and sending a voice message is the same
 * contract with a different middle, so it becomes another page with this same
 * signature rather than a second path through the chat page.
 *
 * ------------------------------------------------------------------------
 * Chinese input is LVGL's pinyin IME with a dictionary of our own
 * (ui_pinyin_dict.h) -- the built-in one is traditional Chinese and offers 謝
 * for "xie". The candidate bar sits where the quick phrases are, which is not
 * a collision: it appears only while a syllable is being typed, and the quick
 * phrases are for when nothing is.
 *
 * ------------------------------------------------------------------------
 * The layout rule this page had to learn the hard way.
 *
 * Everything here is either a plain flex child with a definite size, or
 * explicitly out of the layout. Nothing is aligned by hand into a container
 * that is also laying its children out, and nothing sizes itself as a
 * percentage of something that is sizing itself from its children.
 *
 * That is not tidiness. lv_ime_pinyin_set_keyboard() finishes by calling
 * lv_obj_align_to(), which calls lv_obj_update_layout(), which is
 *
 *     while(scr->scr_layout_inv) { scr->scr_layout_inv = 0; ... }
 *
 * in lv_obj_pos.c -- no iteration cap, no assert, no warning. A size that
 * cannot settle does not produce a wrong layout, it produces a task that
 * never returns. Since that task is the UI task and it is holding the display
 * lock, LVGL then stops repainting AND stops reading the touch panel: the
 * board looks dead, with even the back button gone. The first version of this
 * file did exactly that, on the line that attaches the keyboard to the IME.
 */

#include <string.h>

#include "ui_page.h"
#include "ui_pinyin_dict.h"

/* Phrases worth one tap. Short, and the ones a board on a desk actually
 * needs to answer with -- everything else is what the keyboard is for. */
static const char *const PHRASES[] = {
    "你好", "在", "收到", "好的", "谢谢", "稍等", "我在路上了", "晚点回复你",
};
#define PHRASE_COUNT (sizeof(PHRASES) / sizeof(PHRASES[0]))

#define TEXT_MAX 240

/* The rows above the keyboard. The keyboard itself takes what is left, which
 * on a 240 px panel is about 130 -- four rows of comfortable key. */
#define TOP_H   48
#define CHIPS_H 30
#define CAND_H  30

static char             s_title[UI_TITLE_MAX];
static ui_compose_cb_t  s_on_send;
static lv_obj_t        *s_ta;

/* --------------------------------------------------------------- actions */

static void do_send(void)
{
    const char *text = lv_textarea_get_text(s_ta);

    if (text == NULL || text[0] == '\0') {
        return;
    }

    /* Copied before navigating: ui_back() deletes this page, and with it the
     * textarea the string lives in. */
    char body[TEXT_MAX];

    strlcpy(body, text, sizeof(body));
    if (s_on_send != NULL) {
        s_on_send(body);
    }
    ui_back();
}

static void send_clicked(lv_event_t *e)
{
    (void)e;
    do_send();
}

/* The keyboard's own OK and close keys, so the obvious gesture works as well
 * as the button. */
static void kb_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        do_send();
    } else {
        ui_back();
    }
}

static void phrase_clicked(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (index < PHRASE_COUNT) {
        lv_textarea_add_text(s_ta, PHRASES[index]);
    }
}

/* ------------------------------------------------------------ the page */

void ui_compose_open(const char *title, ui_compose_cb_t on_send)
{
    strlcpy(s_title, title != NULL ? title : "", sizeof(s_title));
    s_on_send = on_send;
    ui_goto(UI_PAGE_COMPOSE);
}

static void create(lv_obj_t *parent)
{
    /* --- what is being written, and the button that sends it --- */
    lv_obj_t *top = lv_obj_create(parent);

    ui_style_flat(top);
    lv_obj_set_size(top, LV_PCT(100), TOP_H);
    lv_obj_set_style_pad_all(top, 4, 0);
    lv_obj_set_style_pad_column(top, 4, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);

    s_ta = lv_textarea_create(top);
    lv_obj_set_height(s_ta, LV_PCT(100));
    lv_obj_set_flex_grow(s_ta, 1);
    lv_textarea_set_max_length(s_ta, TEXT_MAX - 1);
    lv_textarea_set_placeholder_text(s_ta, "说点什么");
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_border_width(s_ta, 0, 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(UI_C_TEXT), 0);
    lv_obj_set_style_pad_all(s_ta, 4, 0);

    lv_obj_t *send = lv_button_create(top);
    lv_obj_set_size(send, 56, LV_PCT(100));
    lv_obj_set_style_bg_color(send, lv_color_hex(UI_C_ACCENT), 0);
    lv_obj_set_style_radius(send, 6, 0);
    lv_obj_set_style_shadow_width(send, 0, 0);
    lv_obj_add_event_cb(send, send_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_label = lv_label_create(send);
    lv_label_set_text(send_label, "发送");
    lv_obj_set_style_text_color(send_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(send_label);

    /* --- one tap each --- */
    lv_obj_t *chips = lv_obj_create(parent);

    ui_style_flat(chips);
    lv_obj_set_size(chips, LV_PCT(100), CHIPS_H);
    lv_obj_set_style_pad_hor(chips, 4, 0);
    lv_obj_set_style_pad_column(chips, 4, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_add_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);

    for (size_t i = 0; i < PHRASE_COUNT; i++) {
        lv_obj_t *chip = lv_button_create(chips);

        lv_obj_set_height(chip, 24);
        lv_obj_set_style_bg_color(chip, lv_color_hex(UI_C_RAISED), 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(chip, 0, 0);
        lv_obj_set_style_pad_hor(chip, 8, 0);
        lv_obj_set_style_pad_ver(chip, 0, 0);
        lv_obj_add_event_cb(chip, phrase_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(chip);
        lv_label_set_text(label, PHRASES[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_C_TEXT), 0);
        lv_obj_center(label);
    }

    /* --- the keyboard, and the IME driving it --- */
    lv_obj_t *kb = lv_keyboard_create(parent);

    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, NULL);

    /* Created after the keyboard so the candidate bar draws over the quick
     * phrases rather than under them. */
    lv_obj_t *ime  = lv_ime_pinyin_create(parent);
    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(ime);

    /* Both come out of the flex column before anything asks LVGL to lay the
     * page out -- see the note at the top of this file. The IME object is a
     * controller and draws nothing, so it goes to zero; the candidate bar is
     * positioned against the keyboard, so it gets a size in pixels rather
     * than the LV_PCT(5) of its parent its constructor gave it. */
    lv_obj_add_flag(ime, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(cand, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(ime, 0, 0);
    lv_obj_set_size(cand, LV_HOR_RES, CAND_H);
    lv_obj_set_style_bg_color(cand, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cand, 2, 0);

    /* lv_ime_pinyin_set_dict() takes a non-const pointer and only reads, so
     * the table stays in flash instead of costing 17 KB of RAM. */
    lv_ime_pinyin_set_dict(ime, (lv_pinyin_dict_t *)ui_pinyin_dict);

    /* set_keyboard() before set_mode(), and the order is not stylistic:
     * set_mode() asserts on pinyin_ime->kb != NULL (lv_ime_pinyin.c:453), and
     * LVGL's assert handler is `while(1);`. Getting it the wrong way round
     * does not fail the call, it parks the calling task -- here the UI task,
     * holding the display lock -- so the whole panel stops repainting and
     * stops reading touch. Silently, too, unless CONFIG_LV_USE_LOG is on,
     * which is why sdkconfig.defaults now turns it on. */
    lv_ime_pinyin_set_keyboard(ime, kb);
    lv_ime_pinyin_set_mode(ime, LV_IME_PINYIN_MODE_K26);

    /* Bring the caret up without making anyone tap the field first. */
    lv_obj_add_state(s_ta, LV_STATE_FOCUSED);
}

static void refresh(uint32_t dirty)
{
    (void)dirty;   /* nothing here comes from the store */
}

static void destroy(void)
{
    s_ta = NULL;
}

static void title(char *buf, size_t buf_size)
{
    lv_snprintf(buf, (uint32_t)buf_size, "发给 %s", s_title);
}

const ui_page_def_t ui_page_compose = {
    .create  = create,
    .refresh = refresh,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
