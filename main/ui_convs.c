/* 会话 -- the conversation list, and the way into a chat.
 *
 * The list is a projection of the store, so a redraw is a re-read: the rows
 * are deleted and rebuilt rather than reconciled. At a dozen rows that is
 * both faster to run and much easier to get right than matching rows to
 * widgets, and it means there is no state here that can drift from what the
 * store holds. Everything the row needs -- the name, the last line, the
 * unread count -- is already on the conversation entry that P4 maintains.
 *
 * The list also does the one thing on this board that changes something the
 * ACCOUNT holds rather than something the board holds: a long press pins or
 * mutes a conversation, which is a user setting (UG/UP) and therefore shows
 * up on the phone too. That is why the press only parks what was asked for
 * and prime() sends it -- the same rule as the composer, for the same reason:
 * an LVGL callback holds the display lock and must not reach the network.
 *
 * The one subtlety is the two-pass shape of refresh(). Names come from the
 * profile caches, and asking for a profile that is not cached makes the
 * client queue a UPUI -- which takes the client's own lock. The walk that
 * produces the rows runs inside the store's lock, and taking the client's
 * lock while holding the store's is the one order the client forbids (see
 * ASSESSMENT.md section 8.6). So the walk only copies rows out, and the
 * asking happens afterwards, with no lock held. The names that arrive land
 * as a user-infos event, which sets UI_DIRTY_NAMES, which redraws this list
 * with the names filled in. No screen ever waits for the server.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ui_page.h"

static const char *TAG = "ui_convs";

/* More than a 320x240 panel shows without scrolling past the point of
 * usefulness, and every row is four LVGL objects. */
#define CONVS_MAX 16

typedef struct {
    wfc_conversation_info_t info;
    char                    title[WFC_NAME_MAX];
    char                    digest[WFC_NAME_MAX + WFC_DIGEST_MAX + 4];
} row_t;

static lv_obj_t *s_list;
static row_t     s_rows[CONVS_MAX];
static size_t    s_count;

/* The long-press menu, and what it asked for. The menu is a child of
 * lv_layer_top() rather than of the list, so rebuilding the rows underneath
 * it does not delete it out from under the finger on it -- which means this
 * file has to delete it itself, at every exit: a button, a tap outside,
 * a redraw, leaving the page. */
static lv_obj_t          *s_menu;
static wfc_conversation_t s_menu_conv;

/* Parked by a menu button, drained by prime(). The conversation is copied
 * rather than kept as a row index: by the time prime() runs, a message may
 * have arrived and reordered the list. */
static struct {
    wfc_conversation_t conv;
    bool               pending;
    bool               is_top;    /* which of the two settings */
    bool               value;
} s_wanted;

/* --------------------------------------------------------------- reading */

/* Runs with the store's lock held: copies, resolves nothing that can fetch,
 * and returns. */
static bool collect(const wfc_conversation_info_t *info, void *ud)
{
    (void)ud;

    row_t *row = &s_rows[s_count];

    row->info = *info;
    wfc_get_conversation_title(&info->conversation, row->title, sizeof(row->title));

    /* "张三: 在路上了" -- in a group, who said it matters as much as what
     * they said. */
    if (info->last_direction != WFC_DIRECTION_SEND &&
        info->conversation.type == WFC_CONV_GROUP && info->last_from[0] != '\0') {
        char who[WFC_NAME_MAX];

        wfc_get_display_name(info->last_from, info->conversation.target, who,
                             sizeof(who));
        snprintf(row->digest, sizeof(row->digest), "%s: %s", who, info->digest);
    } else if (info->last_direction == WFC_DIRECTION_SEND) {
        snprintf(row->digest, sizeof(row->digest), "我: %s", info->digest);
    } else {
        wfc_copy_text(row->digest, sizeof(row->digest), info->digest);
    }

    return ++s_count < CONVS_MAX;
}

/* The page's prime(): no store lock, no display lock, so this may block. A
 * conversation whose name is still an ID in angle brackets is one whose
 * profile is not cached, and asking is what fills it in a moment from now --
 * as a user-infos event, which sets UI_DIRTY_NAMES, which redraws the row. */
static void prime(void)
{
    /* The one blocking thing this page does that is not a fetch. Sent first,
     * so a pin the user just asked for is not queued behind a dozen profile
     * lookups. Nothing is written locally here: the client files the setting
     * when the server acknowledges it and raises the events that redraw this
     * list, so a rejected change simply does not happen. */
    if (s_wanted.pending) {
        esp_err_t err = s_wanted.is_top
                            ? wfc_set_conversation_top(&s_wanted.conv, s_wanted.value)
                            : wfc_set_conversation_silent(&s_wanted.conv,
                                                          s_wanted.value);

        s_wanted.pending = false;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s failed: %s", s_wanted.is_top ? "置顶" : "免打扰",
                     esp_err_to_name(err));
        }
    }

    for (size_t i = 0; i < s_count; i++) {
        const wfc_conversation_t *conv = &s_rows[i].info.conversation;

        if (conv->type == WFC_CONV_GROUP) {
            wfc_group_info_t group;

            wfc_get_group_info(conv->target, false, &group);
        } else {
            wfc_user_info_t user;

            wfc_get_user_info(conv->target, false, &user);
        }
    }
}

/* --------------------------------------------------------------- drawing */

static void row_clicked(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (index < s_count) {
        ui_chat_open(&s_rows[index].info.conversation);
    }
}

/* ----------------------------------------------------- the long-press menu */

static void close_menu(void)
{
    if (s_menu != NULL) {
        lv_obj_delete(s_menu);
        s_menu = NULL;
    }
}

/* Every button in the menu ends here: park what was asked for, close, and
 * ask for a repaint so the UI task reaches prime() and sends it. `top` picks
 * which of the two settings; the new value is the negation of what the row
 * showed, which was read when the menu was built. */
static void menu_choice(lv_event_t *e)
{
    uintptr_t choice = (uintptr_t)lv_event_get_user_data(e);

    s_wanted.conv    = s_menu_conv;
    s_wanted.is_top  = (choice & 0x2) != 0;
    s_wanted.value   = (choice & 0x1) != 0;
    s_wanted.pending = true;

    close_menu();
    ui_dirty(UI_DIRTY_CONVS);
}

static void menu_dismissed(lv_event_t *e)
{
    /* Only a tap on the backdrop itself, not one that bubbled up from a
     * button inside the card. */
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        close_menu();
    }
}

static lv_obj_t *menu_button(lv_obj_t *parent, const char *text, uintptr_t choice)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_add_event_cb(btn, menu_choice, LV_EVENT_CLICKED, (void *)choice);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_C_TEXT), 0);
    lv_obj_center(label);
    return btn;
}

/* Held down on a row: pin it, mute it, or neither. Two settings and a way
 * out, which is the whole of what this board can say about a conversation --
 * a delete would be an MD and a read receipt an RDP, neither of which exists
 * here yet.
 *
 * On lv_layer_top() so that the list rebuilding underneath does not take it
 * away mid-press; see the note where s_menu is declared. */
static void row_held(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (index >= s_count) {
        return;
    }
    close_menu();

    const wfc_conversation_info_t *info = &s_rows[index].info;

    s_menu_conv = info->conversation;

    /* A full-screen backdrop that eats the tap that dismisses it. */
    s_menu = lv_obj_create(lv_layer_top());
    ui_style_flat(s_menu);
    lv_obj_set_size(s_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_50, 0);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_menu, menu_dismissed, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = ui_column(s_menu, 6);
    lv_obj_set_width(card, LV_PCT(70));
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_center(card);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_text(title, s_rows[index].title);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_C_DIM), 0);

    /* Bit 1 of the choice is which setting, bit 0 is the value it should
     * take -- so each button says what it will do, not what is true now. */
    menu_button(card, info->top > 0 ? "取消置顶" : "置顶", info->top > 0 ? 0x2 : 0x3);
    menu_button(card, info->silent ? "取消免打扰" : "免打扰",
                info->silent ? 0x0 : 0x1);
}

static void draw_row(size_t index)
{
    const row_t *data   = &s_rows[index];
    uint32_t     unread = wfc_conversation_unread_total(&data->info);

    lv_obj_t *row = ui_column(s_list, 2);

    lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_RAISED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    lv_obj_add_event_cb(row, row_held, LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);

    /* Title, then when it last had traffic. */
    lv_obj_t *head = lv_obj_create(row);
    ui_style_flat(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(head, 6, 0);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(head);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, data->title);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_C_TEXT), 0);

    /* Pinned rows are already at the top -- the store sorts them there -- so
     * this says WHY they are, which is the part the order alone cannot. */
    if (data->info.top > 0) {
        lv_obj_t *pin = lv_label_create(head);

        lv_obj_set_style_text_color(pin, lv_color_hex(UI_C_ACCENT), 0);
        lv_label_set_text(pin, "顶");
    }

    char when[24];
    ui_format_time(data->info.timestamp, when, sizeof(when));
    lv_obj_t *stamp = lv_label_create(head);
    lv_obj_set_style_text_font(stamp, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(stamp, lv_color_hex(UI_C_DIM), 0);
    lv_label_set_text(stamp, when);

    /* The last line, then the badge. */
    lv_obj_t *body = lv_obj_create(row);
    ui_style_flat(body);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 6, 0);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *digest = lv_label_create(body);
    lv_label_set_long_mode(digest, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(digest, 1);
    lv_label_set_text(digest, data->digest);
    lv_obj_set_style_text_color(digest, lv_color_hex(UI_C_DIM), 0);

    if (unread > 0) {
        lv_obj_t *badge = lv_label_create(body);

        lv_label_set_text_fmt(badge, " %u ", (unsigned)unread);
        lv_obj_set_style_text_font(badge, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(badge, lv_color_hex(0xFFFFFF), 0);
        /* Muting stops the alert, not the counting: the number is the same
         * one, drawn in the colour of something that is not asking to be
         * looked at. */
        lv_obj_set_style_bg_color(
            badge, lv_color_hex(data->info.silent ? UI_C_LINE : UI_C_BAD), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(badge, 2, 0);
    } else if (data->info.silent) {
        /* With nothing unread there is no badge to recolour, so the mute has
         * to say so itself -- otherwise the only way to tell a muted
         * conversation is to long-press it and read the menu. */
        lv_obj_t *muted = lv_label_create(body);

        lv_obj_set_style_text_color(muted, lv_color_hex(UI_C_LINE), 0);
        lv_label_set_text(muted, "静音");
    }
}

/* ------------------------------------------------------------ the page */

static void create(lv_obj_t *parent)
{
    s_list = lv_obj_create(parent);
    ui_style_flat(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_CONVS | UI_DIRTY_NAMES)) == 0) {
        return;
    }

    /* The rows are about to be deleted and rebuilt, so the indices the menu's
     * buttons were built against are gone. It holds a conversation rather
     * than an index, so it would still do the right thing -- but a menu that
     * outlives the row it was opened on reads as a bug, and closing it is the
     * confirmation that the tap landed. */
    close_menu();

    s_count = 0;
    wfc_get_conversations(CONVS_MAX, collect, NULL);

    lv_obj_clean(s_list);
    if (s_count == 0) {
        lv_obj_t *empty = ui_label(s_list, "还没有会话，先从手机发一条消息过来",
                                   UI_C_DIM);

        lv_obj_set_style_pad_all(empty, 12, 0);
    }
    for (size_t i = 0; i < s_count; i++) {
        draw_row(i);
    }

    /* One line that shows both halves of the read model working: a name that
     * resolved out of the profile cache, and an unread count that survived a
     * reboot. It is the P4 acceptance line, kept because it is still the
     * quickest way to tell from a serial console that the panel is right. */
    ESP_LOGI(TAG, "conversations: %u, unread %u%s%s%s", (unsigned)s_count,
             (unsigned)wfc_get_unread_count(), s_count > 0 ? " (top: " : "",
             s_count > 0 ? s_rows[0].title : "", s_count > 0 ? ")" : "");
}

static void destroy(void)
{
    /* Not a child of this page's tree, so leaving the page does not take it
     * with it. Nothing else deletes it. */
    close_menu();
    s_list  = NULL;
    s_count = 0;
}

static void title(char *buf, size_t buf_size)
{
    uint32_t unread = wfc_get_unread_count();

    if (unread > 0) {
        snprintf(buf, buf_size, "会话 · %u", (unsigned)unread);
    } else {
        snprintf(buf, buf_size, "会话");
    }
}

const ui_page_def_t ui_page_convs = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = true,
};
