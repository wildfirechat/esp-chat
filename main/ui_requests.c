/* 新的好友 -- the friend requests, and the two buttons that answer one.
 *
 * The list is FRP's table read straight back (wfc_get_friend_requests), which
 * carries both directions and answered ones too. The wire has no direction
 * field, so which side of a request this board is on is one comparison:
 * from_uid against wfc_client_user_id(). Incoming and unanswered is the only
 * kind with buttons; everything else is a line saying what happened to it.
 *
 * Answering is a write, so it follows the rule the composer's send follows:
 * the button parks what was asked for and prime() sends it a tick later, off
 * the display lock (ui_page.h). Nothing is drawn as answered before the
 * server agrees -- the client files the new status when the reply lands and
 * raises the event that redraws this page, so a refusal simply does not
 * change anything here.
 *
 * Names come out of the profile cache exactly as they do on 联系人, with the
 * same two-pass arrangement: collect() runs under the store's lock and uses
 * the lookup that is documented never to fetch, prime() asks for the ones
 * that came back in angle brackets.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ui_page.h"
#include "wfc_mem.h"

static const char *TAG = "ui_requests";

/* More than a board will have. A request is 200 bytes of row, so this lives
 * in PSRAM like the other lists. */
#define REQUESTS_MAX 32

typedef struct {
    char    uid[WFC_TARGET_MAX];    /* the other party, whichever end that is */
    char    name[WFC_NAME_MAX];
    char    reason[WFC_NAME_MAX];
    int32_t status;
    int64_t update_dt;
    bool    incoming;
} row_t;

static lv_obj_t *s_list;
static row_t    *s_rows;
static size_t    s_count;
static bool      s_ask_names;

/* Parked by a button, drained by prime(). One at a time: two taps in the same
 * 80 ms tick are not something a fingertip does, and the second would only
 * overwrite the first anyway. */
static struct {
    char uid[WFC_TARGET_MAX];
    bool pending;
    bool accept;
} s_wanted;

/* --------------------------------------------------------------- reading */

static bool collect(const wfc_friend_request_t *entry, void *ud)
{
    (void)ud;

    row_t      *row = &s_rows[s_count];
    const char *me  = wfc_client_user_id();

    row->incoming = strcmp(entry->from_uid, me) != 0;
    strlcpy(row->uid, row->incoming ? entry->from_uid : entry->to_uid,
            sizeof(row->uid));
    wfc_copy_text(row->reason, sizeof(row->reason), entry->reason);
    row->status    = entry->status;
    row->update_dt = entry->update_dt;
    wfc_get_display_name(row->uid, NULL, row->name, sizeof(row->name));

    return ++s_count < REQUESTS_MAX;
}

static bool unresolved(const row_t *row)
{
    return row->name[0] == '<';
}

/* Neither backend sorts, and the order FRP happened to deliver in is not one
 * anybody reads a list in. Newest first, with the ones still waiting for an
 * answer above everything else -- those are the reason the page exists.
 *
 * A locally written row has update_dt 0 (the client does not invent a
 * version, see wfc_client.h), so a request just sent from this board sorts to
 * the bottom of the answered ones until the next FRP brings the real stamp.
 * That is the honest place for it: it is the one row on this page nothing is
 * waiting on. */
static bool before(const row_t *a, const row_t *b)
{
    bool a_open = a->incoming && a->status == WFC_FRIEND_RQ_PENDING;
    bool b_open = b->incoming && b->status == WFC_FRIEND_RQ_PENDING;

    if (a_open != b_open) {
        return a_open;
    }
    return a->update_dt > b->update_dt;
}

static void sort_rows(void)
{
    for (size_t i = 1; i < s_count; i++) {
        row_t  row = s_rows[i];
        size_t j   = i;

        while (j > 0 && before(&row, &s_rows[j - 1])) {
            s_rows[j] = s_rows[j - 1];
            j--;
        }
        s_rows[j] = row;
    }
}

static void prime(void)
{
    if (s_rows == NULL) {
        return;
    }

    /* Sent first, so an answer the user just gave is not queued behind a
     * dozen profile lookups. */
    if (s_wanted.pending) {
        esp_err_t err = wfc_handle_friend_request(s_wanted.uid, s_wanted.accept,
                                                  NULL, NULL);

        s_wanted.pending = false;
        if (err != ESP_OK) {
            ui_logf(UI_LOG_ERROR, "好友请求未能提交: %s", esp_err_to_name(err));
        }
    }

    if (!s_ask_names) {
        return;
    }
    s_ask_names = false;

    for (size_t i = 0; i < s_count; i++) {
        if (unresolved(&s_rows[i])) {
            wfc_user_info_t user;

            wfc_get_user_info(s_rows[i].uid, false, &user);
        }
    }
}

/* --------------------------------------------------------------- drawing */

/* Bit 0 is the answer; the rest is the row. Packed into the user data rather
 * than kept in a struct per button because the buttons are rebuilt on every
 * repaint and the pointer would have to be freed with them. */
static void answer_clicked(lv_event_t *e)
{
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(e);
    size_t    index  = packed >> 1;

    if (index >= s_count) {
        return;
    }
    strlcpy(s_wanted.uid, s_rows[index].uid, sizeof(s_wanted.uid));
    s_wanted.accept  = (packed & 1) != 0;
    s_wanted.pending = true;

    ui_dirty(UI_DIRTY_REQUESTS);
}

static void name_clicked(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (index < s_count) {
        ui_contact_open(s_rows[index].uid);
    }
}

static const char *status_text(const row_t *row)
{
    switch (row->status) {
    case WFC_FRIEND_RQ_ACCEPTED:
        return "已同意";
    case WFC_FRIEND_RQ_REJECTED:
        return "已拒绝";
    default:
        return row->incoming ? "等待处理" : "等待对方处理";
    }
}

static void answer_button(lv_obj_t *parent, const char *text, uint32_t colour,
                          uintptr_t packed)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, 56, 26);
    lv_obj_set_style_bg_color(btn, lv_color_hex(colour), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, answer_clicked, LV_EVENT_CLICKED, (void *)packed);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
}

static void draw_row(size_t index)
{
    const row_t *data = &s_rows[index];

    lv_obj_t *row = lv_obj_create(s_list);

    ui_style_flat(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* The name and the reason, and tapping them leads to the person -- which
     * is how someone decides whether to say yes to a name they half know. */
    lv_obj_t *text = ui_column(row, 2);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_width(text, 0);
    lv_obj_add_flag(text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(text, name_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);

    lv_obj_t *name = lv_label_create(text);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_text(name, data->name);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_C_TEXT), 0);

    lv_obj_t *note = lv_label_create(text);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_color(note, lv_color_hex(UI_C_DIM), 0);
    lv_obj_set_style_text_font(note, UI_FONT_SMALL, 0);
    if (data->reason[0] != '\0') {
        lv_label_set_text(note, data->reason);
    } else {
        lv_label_set_text(note, status_text(data));
    }

    if (data->incoming && data->status == WFC_FRIEND_RQ_PENDING) {
        answer_button(row, "同意", UI_C_ACCENT, (uintptr_t)index << 1 | 1);
        answer_button(row, "拒绝", UI_C_RAISED, (uintptr_t)index << 1);
        return;
    }

    lv_obj_t *state = lv_label_create(row);
    lv_label_set_text(state, status_text(data));
    lv_obj_set_style_text_font(state, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(
        state,
        lv_color_hex(data->status == WFC_FRIEND_RQ_ACCEPTED ? UI_C_OK : UI_C_DIM), 0);
}

/* ------------------------------------------------------------ the page */

static void create(lv_obj_t *parent)
{
    s_rows      = wfc_calloc(REQUESTS_MAX, sizeof(*s_rows));
    s_count     = 0;
    s_ask_names = false;

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
    if ((dirty & (UI_DIRTY_REQUESTS | UI_DIRTY_FRIENDS | UI_DIRTY_NAMES)) == 0) {
        return;
    }
    if (s_rows == NULL) {
        return;
    }

    s_count = 0;
    wfc_get_friend_requests(REQUESTS_MAX, collect, NULL);
    sort_rows();

    lv_obj_clean(s_list);
    if (s_count == 0) {
        lv_obj_t *empty = ui_label(s_list, "没有好友请求", UI_C_DIM);

        lv_obj_set_style_pad_all(empty, 12, 0);
    }
    s_ask_names = false;
    for (size_t i = 0; i < s_count; i++) {
        draw_row(i);
        s_ask_names = s_ask_names || unresolved(&s_rows[i]);
    }

    ESP_LOGI(TAG, "friend requests: %u", (unsigned)s_count);
}

static void destroy(void)
{
    wfc_free(s_rows);
    s_rows      = NULL;
    s_list      = NULL;
    s_count     = 0;
    s_ask_names = false;
}

static void title(char *buf, size_t buf_size)
{
    size_t waiting = 0;

    for (size_t i = 0; i < s_count; i++) {
        if (s_rows[i].incoming && s_rows[i].status == WFC_FRIEND_RQ_PENDING) {
            waiting++;
        }
    }
    if (waiting > 0) {
        snprintf(buf, buf_size, "新的好友 · %u", (unsigned)waiting);
    } else {
        strlcpy(buf, "新的好友", buf_size);
    }
}

const ui_page_def_t ui_page_requests = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
