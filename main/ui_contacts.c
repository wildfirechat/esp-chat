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

/* The page's prime(): no store lock, no display lock, so this may block. */
static void prime(void)
{
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

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_FRIENDS | UI_DIRTY_NAMES)) == 0) {
        return;
    }
    if (s_rows == NULL) {
        return;
    }

    s_count = 0;
    wfc_get_friends(CONTACTS_MAX, collect, NULL);
    sort_rows();

    lv_obj_clean(s_list);
    if (s_count == 0) {
        /* The friend list is synced, not built here: this client reads FP and
         * FRP and cannot answer a request (wfc_client.h), so the way to get a
         * row on this screen is to add someone from a phone. */
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
