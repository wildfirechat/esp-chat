/* 联系人 -- the friend list, and the way into one person.
 *
 * Same shape as the conversation list and for the same reasons: the store is
 * the model, a redraw is a re-read, and rows are rebuilt rather than
 * reconciled. What differs is where a row's text comes from. The friend table
 * holds the relationship and nothing else -- an ID, what I call them, whether
 * it still stands -- so every name on this screen comes out of the profile
 * cache, and on a board that has just booted that cache is empty. Which makes
 * this the page where ui_page.h's two-pass rule matters most: collect() runs
 * under the store's lock and names each row with the lookup that is
 * documented never to fetch, and prime() asks for the ones that came back in
 * angle brackets with no lock held. The profiles land as a user-infos event,
 * which sets UI_DIRTY_NAMES, which redraws the list with the names filled in.
 *
 * The asking is done once per redraw rather than once per prime(), because
 * prime() runs on every tick that repainted anything -- the clock alone is
 * one a second -- and a profile the server will not answer for would
 * otherwise be a UPUI every second for as long as the page is up. A friend
 * arriving or a name landing redraws the list, and that is exactly when
 * asking again is worth it.
 *
 * Two rows above the people are not people: 新的好友 leads to the requests
 * page with a badge for the ones still waiting, and 发起群聊 opens the picker
 * and makes a group out of what comes back. Both live here because this is
 * where the address book is, and neither knows anything about the other.
 *
 * The order is this page's own. Neither store backend sorts, and the order FP
 * happened to deliver in is not one anybody can find a name in, so the rows
 * are sorted after the walk -- by UTF-8 bytes, which is codepoint order: A to
 * Z for Latin names, stable but arbitrary for Chinese ones. Sorting Chinese
 * properly means a hanzi-to-pinyin table, which is not what the IME's
 * dictionary is (that one goes the other way, syllables to characters), and a
 * second table is not worth it to order a list of a dozen people.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ui_page.h"
#include "wfc_mem.h"
#include "wfc_store.h"

static const char *TAG = "ui_contacts";

/* Rows read per redraw. The list scrolls, so this is "more than a board will
 * have" rather than a screenful; at 128 bytes a row it lives in PSRAM like
 * the chat page's messages rather than in a static array. */
#define CONTACTS_MAX 64

typedef struct {
    char uid[WFC_TARGET_MAX];
    char name[WFC_NAME_MAX];
} row_t;

static lv_obj_t *s_list;
static row_t    *s_rows;
static size_t    s_count;
/* Set by refresh() when it drew a row that is still an ID, drained by
 * prime(). */
static bool      s_ask_names;

/* How many requests are waiting for an answer, for the badge on the 新的好友
 * row. Counted during the same repaint that reads the friends, because both
 * are store walks and the row has to say something. */
static size_t    s_waiting;

/* A group the user asked for, parked by the picker's callback and drained by
 * prime() -- the same arrangement the chat page has with the composer, and
 * for the same reason: the callback runs on the LVGL task with the display
 * lock held. */
static struct {
    char   members[UI_PICK_MAX][WFC_TARGET_MAX];
    char   name[WFC_NAME_MAX];
    size_t n;
} s_new_group;

/* --------------------------------------------------------------- reading */

/* Runs with the store's lock held: copies the ID out, resolves the name with
 * the call that cannot reach the network, and returns. */
static bool collect(const wfc_friend_t *entry, void *ud)
{
    (void)ud;

    row_t *row = &s_rows[s_count];

    strlcpy(row->uid, entry->uid, sizeof(row->uid));
    /* Not in a group, so this is the friend alias if I set one and their
     * nickname otherwise -- the same precedence every other client uses. */
    wfc_get_display_name(entry->uid, NULL, row->name, sizeof(row->name));

    return ++s_count < CONTACTS_MAX;
}

/* An unresolved ID reads as "<uEPhwEwgg>" (wfc_client.h), and '<' sorts below
 * every letter -- so without this the rows nobody can read are the ones at
 * the top of the list. */
static bool unresolved(const row_t *row)
{
    return row->name[0] == '<';
}

static bool before(const row_t *a, const row_t *b)
{
    if (unresolved(a) != unresolved(b)) {
        return unresolved(b);
    }
    return strcmp(a->name, b->name) < 0;
}

/* Insertion sort: CONTACTS_MAX is 64 and the list is nearly always already
 * short, which is the case this beats a real sort at without needing a
 * comparison function that takes a void pointer. */
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

/* Made when a group is created and nobody typed a name, which on this panel
 * is always: WFC has no server-side default, so every client composes one out
 * of who is in it, and this is that -- the first few display names, joined.
 *
 * It goes out as the group's real name, which is what makes it worth doing
 * properly: cut on a character boundary like every other stored string, and
 * with the count when there are more people than fit. */
static void compose_group_name(char *buf, size_t buf_size, const char *const *uids,
                               size_t n)
{
    char   part[WFC_NAME_MAX];
    size_t used = 0;

    buf[0] = '\0';
    for (size_t i = 0; i < n && i < 3; i++) {
        wfc_get_display_name(uids[i], NULL, part, sizeof(part));
        used += (size_t)snprintf(buf + used, buf_size - used, "%s%s",
                                 used > 0 ? "、" : "", part);
        if (used >= buf_size) {
            break;
        }
    }
    if (n > 3 && used + 8 < buf_size) {
        snprintf(buf + used, buf_size - used, " 等 %u 人", (unsigned)(n + 1));
    }
    /* Every name could have been an unresolved ID, or the buffer could have
     * filled on the first one. Either way something has to go on the wire. */
    if (buf[0] == '\0') {
        strlcpy(buf, "群聊", buf_size);
    }
}

static void picked_for_group(const char *const *uids, size_t n)
{
    s_new_group.n = n < UI_PICK_MAX ? n : UI_PICK_MAX;
    for (size_t i = 0; i < s_new_group.n; i++) {
        strlcpy(s_new_group.members[i], uids[i], WFC_TARGET_MAX);
    }
    compose_group_name(s_new_group.name, sizeof(s_new_group.name), uids,
                       s_new_group.n);
}

/* Made by the client when the server answers, on the wfc_mqtt task -- so this
 * only opens the conversation, which is a queue post like every other
 * navigation. The group is already in the store by the time this runs
 * (wfc_client.h), so the chat page comes up with its name on it. */
static void group_created(int error_code, const char *group_id, void *ud)
{
    (void)ud;

    /* The ID is the test rather than the code: 222 says some of the people
     * asked for did not get in, and a group that exists with fewer people in
     * it than were picked is still a group to open. */
    if (group_id == NULL || group_id[0] == '\0') {
        ui_logf(UI_LOG_ERROR, "建群失败（%d）", error_code);
        return;
    }

    wfc_conversation_t conv = { .type = WFC_CONV_GROUP, .line = 0 };

    strlcpy(conv.target, group_id, sizeof(conv.target));
    ui_chat_open(&conv);
}

/* The page's prime(): no store lock, no display lock, so this may block. */
static void prime(void)
{
    if (s_new_group.n > 0) {
        const char *ids[UI_PICK_MAX];

        for (size_t i = 0; i < s_new_group.n; i++) {
            ids[i] = s_new_group.members[i];
        }

        esp_err_t err = wfc_create_group(s_new_group.name, ids, s_new_group.n,
                                         group_created, NULL);

        s_new_group.n = 0;
        if (err != ESP_OK) {
            ui_logf(UI_LOG_ERROR, "建群未能提交: %s", esp_err_to_name(err));
        }
    }

    if (s_rows == NULL || !s_ask_names) {
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

static void row_clicked(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (index < s_count) {
        ui_contact_open(s_rows[index].uid);
    }
}

/* The two rows above the people, which are not people. Both lead somewhere
 * rather than saying anything, so they are drawn like a contact row with the
 * same arrow -- what differs is that one carries a badge. */
static void requests_clicked(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_PAGE_REQUESTS);
}

static void new_group_clicked(lv_event_t *e)
{
    (void)e;
    ui_pick_open("发起群聊", UI_PICK_FRIENDS, NULL, picked_for_group);
}

static void draw_entry(const char *text, size_t badge, lv_event_cb_t on_click)
{
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
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_RAISED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_C_TEXT), 0);

    if (badge > 0) {
        lv_obj_t *count = lv_label_create(row);
        char      text_buf[12];

        snprintf(text_buf, sizeof(text_buf), "%u", (unsigned)badge);
        lv_label_set_text(count, text_buf);
        lv_obj_set_style_text_font(count, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(count, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(count, lv_color_hex(UI_C_BAD), 0);
        lv_obj_set_style_bg_opa(count, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(count, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(count, 5, 0);
        lv_obj_set_style_pad_ver(count, 1, 0);
    }

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(UI_C_DIM), 0);
}

static void draw_row(size_t index)
{
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
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_RAISED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_text(name, s_rows[index].name);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_C_TEXT), 0);

    /* One line, one arrow: the row's whole job is to lead somewhere, and the
     * facts about the person belong on the page it leads to. */
    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(UI_C_DIM), 0);
}

/* ------------------------------------------------------------ the page */

static void create(lv_obj_t *parent)
{
    s_rows      = wfc_calloc(CONTACTS_MAX, sizeof(*s_rows));
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

/* Runs under the store's lock like collect(), and only counts: which side of
 * a request this board is on is from_uid against our own ID, and the wire
 * carries nothing else to tell them apart (wfc_model.h). */
static bool count_waiting(const wfc_friend_request_t *entry, void *ud)
{
    (void)ud;

    if (entry->status == WFC_FRIEND_RQ_PENDING &&
        strcmp(entry->from_uid, wfc_client_user_id()) != 0) {
        s_waiting++;
    }
    return true;
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_FRIENDS | UI_DIRTY_NAMES | UI_DIRTY_REQUESTS)) == 0) {
        return;
    }
    if (s_rows == NULL) {
        return;
    }

    s_count = 0;
    wfc_get_friends(CONTACTS_MAX, collect, NULL);
    sort_rows();

    s_waiting = 0;
    wfc_get_friend_requests(CONTACTS_MAX, count_waiting, NULL);

    lv_obj_clean(s_list);

    draw_entry("新的好友", s_waiting, requests_clicked);
    draw_entry("发起群聊", 0, new_group_clicked);

    if (s_count == 0) {
        /* Adding people is possible from here now (新的好友 answers requests,
         * a contact page sends one), but a board with an empty address book
         * still has nothing to search: there is no directory page, so the
         * first friend comes from somewhere with a keyboard. */
        lv_obj_t *empty = ui_label(s_list, "还没有好友，先在手机上加一个", UI_C_DIM);

        lv_obj_set_style_pad_all(empty, 12, 0);
    }
    s_ask_names = false;
    for (size_t i = 0; i < s_count; i++) {
        draw_row(i);
        s_ask_names = s_ask_names || unresolved(&s_rows[i]);
    }

    ESP_LOGI(TAG, "contacts: %u%s%s%s", (unsigned)s_count,
             s_count > 0 ? " (top: " : "", s_count > 0 ? s_rows[0].name : "",
             s_count > 0 ? ")" : "");
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
    if (s_count > 0) {
        snprintf(buf, buf_size, "联系人 · %u", (unsigned)s_count);
    } else {
        strlcpy(buf, "联系人", buf_size);
    }
}

const ui_page_def_t ui_page_contacts = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = true,
};
