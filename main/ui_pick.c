/* 选人 -- choosing people, once, for the three flows that need it.
 *
 * Making a group out of contacts, inviting contacts into a group, and taking
 * members out of one are the same screen with a different list behind it and
 * a different thing done with the answer. So this is one page with a source
 * and a callback (ui_page.h), the way the composer is one page for every line
 * of text the panel collects -- and for the same payoff: the three callers
 * are a few lines each and none of them draws a list.
 *
 * The selection is the page's own state and does not survive it. A row
 * carries a tick; 确定 hands the ticked IDs to the caller and goes back. The
 * caller then sends, from its own prime(), because a button callback runs on
 * the LVGL task with the display lock held and calling into the client there
 * is what makes the board look dead (ui_page.h).
 *
 * Names, again, come from the profile cache with the two-pass rule: collect()
 * resolves with the lookup that cannot fetch, prime() asks for what came back
 * in angle brackets.
 */

#include <stdio.h>
#include <string.h>

#include "ui_page.h"
#include "wfc_mem.h"

/* How many people may be offered. Larger than UI_PICK_MAX, which bounds how
 * many may be chosen: a group of thirty is a list worth scrolling even if
 * only a few of them are wanted. */
#define OFFER_MAX 48

typedef struct {
    char uid[WFC_TARGET_MAX];
    char name[WFC_NAME_MAX];
    bool chosen;
} row_t;

/* What the page was opened with. Set before the navigation that builds the
 * widgets, like every other *_open() here. */
static char             s_title[UI_TITLE_MAX];
static ui_pick_source_t s_source;
static char             s_group[WFC_TARGET_MAX];
static ui_pick_cb_t     s_on_done;

static lv_obj_t *s_list;
static lv_obj_t *s_done;
static lv_obj_t *s_done_label;
static row_t    *s_rows;
static size_t    s_count;
static bool      s_ask_names;

/* --------------------------------------------------------------- reading */

static size_t chosen_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < s_count; i++) {
        n += s_rows[i].chosen ? 1 : 0;
    }
    return n;
}

/* Both collectors run under the store's lock. The membership test one of them
 * needs is another store read, which is allowed -- the mutex is recursive --
 * and is the reason 邀请 does not offer people who are already in. */
static bool offer(const char *uid)
{
    if (uid == NULL || uid[0] == '\0' || s_count >= OFFER_MAX) {
        return false;
    }
    if (strcmp(uid, wfc_client_user_id()) == 0) {
        return false;
    }

    row_t *row = &s_rows[s_count];

    strlcpy(row->uid, uid, sizeof(row->uid));
    wfc_get_display_name(uid, s_group[0] != '\0' ? s_group : NULL, row->name,
                         sizeof(row->name));
    s_count++;
    return true;
}

static bool collect_friend(const wfc_friend_t *entry, void *ud)
{
    (void)ud;

    if (s_group[0] != '\0') {
        wfc_group_member_t member;

        /* Already in the group, so there is nothing to invite them to. A row
         * whose type is Removed is not in the store at all (wfc_store.h), so
         * this needs no state test. */
        if (wfc_store_get_group_member(s_group, entry->uid, &member)) {
            return true;
        }
    }
    offer(entry->uid);
    return s_count < OFFER_MAX;
}

static bool collect_member(const wfc_group_member_t *member, void *ud)
{
    (void)ud;

    offer(member->member_id);
    return s_count < OFFER_MAX;
}

static void prime(void)
{
    if (s_rows == NULL || !s_ask_names) {
        return;
    }
    s_ask_names = false;

    for (size_t i = 0; i < s_count; i++) {
        if (s_rows[i].name[0] == '<') {
            wfc_user_info_t user;

            wfc_get_user_info(s_rows[i].uid, false, &user);
        }
    }
}

/* --------------------------------------------------------------- drawing */

static void row_clicked(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (index >= s_count) {
        return;
    }
    /* The cap is enforced here rather than at 确定, so that the tap that
     * would break it does nothing visible instead of the button quietly
     * refusing later. */
    if (!s_rows[index].chosen && chosen_count() >= UI_PICK_MAX) {
        return;
    }
    s_rows[index].chosen = !s_rows[index].chosen;
    ui_dirty(UI_DIRTY_FRIENDS);
}

static void done_clicked(lv_event_t *e)
{
    (void)e;

    const char *uids[UI_PICK_MAX];
    size_t      n = 0;

    for (size_t i = 0; i < s_count && n < UI_PICK_MAX; i++) {
        if (s_rows[i].chosen) {
            uids[n++] = s_rows[i].uid;
        }
    }
    if (n == 0) {
        return;
    }
    if (s_on_done != NULL) {
        /* The caller copies what it wants: these point into rows this page is
         * about to delete. */
        s_on_done(uids, n);
    }
    ui_back();
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

    /* A tick rather than a checkbox: LVGL's checkbox brings its own label and
     * its own layout, and all this needs is one glyph that is there or is
     * not. */
    lv_obj_t *tick = lv_label_create(row);
    lv_obj_set_width(tick, 16);
    lv_label_set_text(tick, s_rows[index].chosen ? LV_SYMBOL_OK : "");
    lv_obj_set_style_text_color(tick, lv_color_hex(UI_C_ACCENT), 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_text(name, s_rows[index].name);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_C_TEXT), 0);
}

/* ------------------------------------------------------------ the page */

void ui_pick_open(const char *title, ui_pick_source_t source, const char *group,
                  ui_pick_cb_t on_done)
{
    strlcpy(s_title, title != NULL ? title : "选人", sizeof(s_title));
    s_source  = source;
    s_on_done = on_done;
    strlcpy(s_group, group != NULL ? group : "", sizeof(s_group));
    ui_goto(UI_PAGE_PICK);
}

static void create(lv_obj_t *parent)
{
    s_rows      = wfc_calloc(OFFER_MAX, sizeof(*s_rows));
    s_count     = 0;
    s_ask_names = false;

    s_list = lv_obj_create(parent);
    ui_style_flat(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    lv_obj_t *bar = lv_obj_create(parent);
    ui_style_flat(bar);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_C_LINE), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 5, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    s_done = lv_button_create(bar);
    lv_obj_set_size(s_done, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_done, lv_color_hex(UI_C_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_done, 0, 0);
    lv_obj_set_style_radius(s_done, 6, 0);
    lv_obj_add_event_cb(s_done, done_clicked, LV_EVENT_CLICKED, NULL);

    s_done_label = lv_label_create(s_done);
    lv_obj_set_style_text_color(s_done_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_done_label, "确定");
    lv_obj_center(s_done_label);
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_FRIENDS | UI_DIRTY_NAMES)) == 0) {
        return;
    }
    if (s_rows == NULL) {
        return;
    }

    /* Which of them were ticked has to survive the re-read, since a profile
     * landing repaints this page and the selection is not in the store. */
    char   chosen[UI_PICK_MAX][WFC_TARGET_MAX];
    size_t n_chosen = 0;

    for (size_t i = 0; i < s_count && n_chosen < UI_PICK_MAX; i++) {
        if (s_rows[i].chosen) {
            strlcpy(chosen[n_chosen++], s_rows[i].uid, WFC_TARGET_MAX);
        }
    }

    s_count = 0;
    memset(s_rows, 0, OFFER_MAX * sizeof(*s_rows));
    if (s_source == UI_PICK_GROUP_MEMBERS) {
        wfc_get_group_members(s_group, OFFER_MAX, collect_member, NULL);
    } else {
        wfc_get_friends(OFFER_MAX, collect_friend, NULL);
    }

    for (size_t i = 0; i < s_count; i++) {
        for (size_t j = 0; j < n_chosen; j++) {
            if (strcmp(s_rows[i].uid, chosen[j]) == 0) {
                s_rows[i].chosen = true;
                break;
            }
        }
    }

    lv_obj_clean(s_list);
    if (s_count == 0) {
        lv_obj_t *empty = ui_label(
            s_list,
            s_source == UI_PICK_GROUP_MEMBERS ? "群成员还没同步过来" : "没有人可选",
            UI_C_DIM);

        lv_obj_set_style_pad_all(empty, 12, 0);
    }
    s_ask_names = false;
    for (size_t i = 0; i < s_count; i++) {
        draw_row(i);
        s_ask_names = s_ask_names || s_rows[i].name[0] == '<';
    }

    size_t picked = chosen_count();
    char   label[24];

    if (picked > 0) {
        snprintf(label, sizeof(label), "确定 · %u", (unsigned)picked);
    } else {
        strlcpy(label, "确定", sizeof(label));
    }
    lv_label_set_text(s_done_label, label);
    if (picked > 0) {
        lv_obj_remove_state(s_done, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_done, LV_STATE_DISABLED);
    }
}

static void destroy(void)
{
    wfc_free(s_rows);
    s_rows       = NULL;
    s_list       = NULL;
    s_done       = NULL;
    s_done_label = NULL;
    s_count      = 0;
    s_ask_names  = false;
}

static void title(char *buf, size_t buf_size)
{
    strlcpy(buf, s_title, buf_size);
}

const ui_page_def_t ui_page_pick = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
