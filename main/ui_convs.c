/* 会话 -- the conversation list, and the way into a chat.
 *
 * The list is a projection of the store, so a redraw is a re-read: the rows
 * are deleted and rebuilt rather than reconciled. At a dozen rows that is
 * both faster to run and much easier to get right than matching rows to
 * widgets, and it means there is no state here that can drift from what the
 * store holds. Everything the row needs -- the name, the last line, the
 * unread count -- is already on the conversation entry that P4 maintains.
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
        lv_obj_set_style_bg_color(badge, lv_color_hex(UI_C_BAD), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(badge, 2, 0);
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
