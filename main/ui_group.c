/* 群 -- one group: who is in it, and the three things this board can do
 * about that.
 *
 * Pushed from a group's chat page and handed nothing but a group ID, which is
 * the whole of its state; everything drawn is read back out of the store at
 * repaint time, so a roster arriving while it is open fills itself in. Same
 * arrangement as the contact page and for the same reasons.
 *
 * The three writes -- invite, remove, leave -- are the group half of
 * wfc_client.h's relationship API. Two of them go through the picker
 * (ui_page.h) and all three are parked by a button and sent from prime(),
 * because a button callback holds the display lock.
 *
 * Whether the 移出 button appears is decided by what the server would allow,
 * which is knowable here and so is worked out rather than guessed at (see
 * i_can_remove). Getting it wrong is not dangerous -- the server refuses and
 * the reply says so -- but a button that always fails is worse than no
 * button. Inviting is offered to everyone, because whether a deployment
 * allows it at all is a server-side setting this board cannot see.
 *
 * Leaving is the one action with a confirmation, because it is the one that
 * cannot be undone from here: this board drops the conversation and its
 * messages with it (wfc_client.h), and getting back in needs somebody else.
 */

#include <stdio.h>
#include <string.h>

#include "ui_page.h"
#include "wfc_mem.h"
#include "wfc_store.h"

/* How much of a roster this page shows. A board is not where a two-hundred
 * person group is administered; past this the list is cut and says so. */
#define MEMBERS_MAX 32

/* What was asked for, parked by a button and drained by prime(). One at a
 * time: these are page-level actions, not per-row ones. */
typedef enum {
    WANT_NOTHING = 0,
    WANT_ADD,
    WANT_KICK,
    WANT_QUIT,
} want_t;

static char s_group[WFC_TARGET_MAX];

static lv_obj_t *s_body;
static lv_obj_t *s_bar;

typedef struct {
    char uid[WFC_TARGET_MAX];
    char name[WFC_NAME_MAX];
    int32_t type;
} row_t;

static lv_obj_t *s_kick;   /* only for the owner and the managers */

static row_t *s_rows;
static size_t s_count;
static bool   s_ask_names;
static bool   s_ask_group;

static struct {
    want_t want;
    char   members[UI_PICK_MAX][WFC_TARGET_MAX];
    size_t n;
} s_wanted;

/* --------------------------------------------------------------- reading */

static bool collect(const wfc_group_member_t *member, void *ud)
{
    (void)ud;

    row_t *row = &s_rows[s_count];

    strlcpy(row->uid, member->member_id, sizeof(row->uid));
    row->type = member->type;
    /* In a group, so the group alias wins over every other name -- which is
     * the whole reason the member table is cached at all. */
    wfc_get_display_name(member->member_id, s_group, row->name, sizeof(row->name));

    return ++s_count < MEMBERS_MAX;
}

/* Who the server will let remove someone, read off KickoffGroupMember.java:
 * the owner, a manager, or anybody at all in a Free group -- that type's whole
 * point is that it is not administered. A Normal or Restricted group refuses
 * everyone else.
 *
 * Read from the store rather than assumed, and re-read on every repaint,
 * because on a board that has just opened a group neither the record nor the
 * roster is cached yet and every answer here is "no" until the GPGI and the
 * GPGM land. */
static bool i_can_remove(void)
{
    wfc_group_info_t   group;
    wfc_group_member_t me;

    if (!wfc_store_get_group(s_group, &group)) {
        return false;
    }
    if (group.type == WFC_GROUP_FREE ||
        strcmp(group.owner, wfc_client_user_id()) == 0) {
        return true;
    }
    return wfc_store_get_group_member(s_group, wfc_client_user_id(), &me) &&
           me.type == WFC_MEMBER_MANAGER;
}

static void prime(void)
{
    if (s_rows == NULL) {
        return;
    }

    /* Sent before the profile lookups, so an action the user just asked for
     * is not queued behind a dozen of them. */
    switch (s_wanted.want) {
    case WANT_ADD:
    case WANT_KICK: {
        const char *ids[UI_PICK_MAX];

        for (size_t i = 0; i < s_wanted.n; i++) {
            ids[i] = s_wanted.members[i];
        }

        esp_err_t err = s_wanted.want == WANT_ADD
                            ? wfc_add_group_members(s_group, ids, s_wanted.n, NULL, NULL)
                            : wfc_kick_group_members(s_group, ids, s_wanted.n, NULL,
                                                     NULL);

        if (err != ESP_OK) {
            ui_logf(UI_LOG_ERROR, "%s未能提交: %s",
                    s_wanted.want == WANT_ADD ? "邀请" : "移出",
                    esp_err_to_name(err));
        }
        break;
    }
    case WANT_QUIT: {
        esp_err_t err = wfc_quit_group(s_group, NULL, NULL);

        /* Nothing navigates away here. The page leaves when the server has
         * actually agreed: the client removes the conversation and raises a
         * conversation-removed event, and the shell takes any page standing
         * on it back one (ui.c). A refusal therefore leaves this page up,
         * which is the honest outcome. */
        if (err != ESP_OK) {
            ui_logf(UI_LOG_ERROR, "退群未能提交: %s", esp_err_to_name(err));
        }
        break;
    }
    default:
        break;
    }
    s_wanted.want = WANT_NOTHING;
    s_wanted.n    = 0;

    if (s_ask_group) {
        wfc_group_info_t group;

        s_ask_group = false;
        /* Asks for the record and, through it, the roster: a group whose
         * member_update_dt has moved past what the member table holds is what
         * makes the client send a GPGM (wfc_client.h). */
        wfc_get_group_info(s_group, false, &group);
    }
    if (!s_ask_names) {
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

/* ---------------------------------------------------------------- actions */

/* Both picker callbacks run on the LVGL task with the display lock held --
 * ui_page.h's rule -- so both only copy. The page they return to is this one,
 * which is rebuilt by then, which is why the selection lives in a static
 * rather than in a widget. */
static void picked_to_add(const char *const *uids, size_t n)
{
    s_wanted.n = n < UI_PICK_MAX ? n : UI_PICK_MAX;
    for (size_t i = 0; i < s_wanted.n; i++) {
        strlcpy(s_wanted.members[i], uids[i], WFC_TARGET_MAX);
    }
    s_wanted.want = WANT_ADD;
}

static void picked_to_kick(const char *const *uids, size_t n)
{
    s_wanted.n = n < UI_PICK_MAX ? n : UI_PICK_MAX;
    for (size_t i = 0; i < s_wanted.n; i++) {
        strlcpy(s_wanted.members[i], uids[i], WFC_TARGET_MAX);
    }
    s_wanted.want = WANT_KICK;
}

static void add_clicked(lv_event_t *e)
{
    (void)e;
    ui_pick_open("邀请入群", UI_PICK_FRIENDS, s_group, picked_to_add);
}

static void kick_clicked(lv_event_t *e)
{
    (void)e;
    ui_pick_open("移出群聊", UI_PICK_GROUP_MEMBERS, s_group, picked_to_kick);
}

/* The confirmation, on lv_layer_top() for the reason ui_convs.c's menu is
 * there: the page underneath is rebuilt on every repaint and would otherwise
 * delete the card out from under the finger on it. */
static lv_obj_t *s_confirm;

static void close_confirm(void)
{
    if (s_confirm != NULL) {
        lv_obj_delete(s_confirm);
        s_confirm = NULL;
    }
}

static void confirm_quit(lv_event_t *e)
{
    (void)e;
    close_confirm();
    s_wanted.want = WANT_QUIT;
    ui_dirty(UI_DIRTY_NAMES);
}

static void confirm_dismissed(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        close_confirm();
    }
}

static void quit_clicked(lv_event_t *e)
{
    (void)e;
    close_confirm();

    s_confirm = lv_obj_create(lv_layer_top());
    ui_style_flat(s_confirm);
    lv_obj_set_size(s_confirm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_confirm, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_50, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_confirm, confirm_dismissed, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = ui_column(s_confirm, 8);
    lv_obj_set_width(card, LV_PCT(76));
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_center(card);

    ui_label(card, "退出后这个会话和里面的消息都会从板子上删掉。", UI_C_DIM);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_BAD), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_add_event_cb(btn, confirm_quit, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "退出群聊");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
}

static void member_clicked(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (index < s_count) {
        ui_contact_open(s_rows[index].uid);
    }
}

/* --------------------------------------------------------------- drawing */

static void draw_member(size_t index)
{
    const row_t *data = &s_rows[index];

    lv_obj_t *row = lv_obj_create(s_body);

    ui_style_flat(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 7, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_C_RAISED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, member_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_text(name, data->name);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_C_TEXT), 0);

    if (data->type == WFC_MEMBER_OWNER || data->type == WFC_MEMBER_MANAGER) {
        lv_obj_t *tag = lv_label_create(row);

        lv_label_set_text(tag, data->type == WFC_MEMBER_OWNER ? "群主" : "管理员");
        lv_obj_set_style_text_font(tag, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(UI_C_ACCENT), 0);
    }
}

static void draw_body(void)
{
    wfc_group_info_t group  = { 0 };
    bool             cached = wfc_store_get_group(s_group, &group);

    s_count = 0;
    wfc_get_group_members(s_group, MEMBERS_MAX, collect, NULL);

    lv_obj_clean(s_body);

    char head[UI_TITLE_MAX + 24];

    snprintf(head, sizeof(head), "群成员 %u 人",
             (unsigned)(group.member_count > 0 ? (unsigned)group.member_count
                                               : (unsigned)s_count));
    lv_obj_t *label = ui_label(s_body, head, UI_C_DIM);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_style_pad_bottom(label, 2, 0);

    for (size_t i = 0; i < s_count; i++) {
        draw_member(i);
    }
    if (s_count == 0) {
        ui_label(s_body, "群成员还没同步过来", UI_C_DIM);
    } else if (s_count == MEMBERS_MAX) {
        lv_obj_t *more = ui_label(s_body, "只显示前 32 位", UI_C_DIM);

        lv_obj_set_style_text_font(more, UI_FONT_SMALL, 0);
    }

    s_ask_group = !cached || s_count == 0;
    s_ask_names = false;
    for (size_t i = 0; i < s_count; i++) {
        s_ask_names = s_ask_names || s_rows[i].name[0] == '<';
    }
}

static lv_obj_t *action(const char *text, uint32_t colour, lv_event_cb_t on_click)
{
    lv_obj_t *btn = lv_button_create(s_bar);

    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_hex(colour), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

/* ------------------------------------------------------------ the page */

void ui_group_open(const char *group_id)
{
    if (group_id == NULL || group_id[0] == '\0') {
        return;
    }
    strlcpy(s_group, group_id, sizeof(s_group));
    s_wanted.want = WANT_NOTHING;
    s_wanted.n    = 0;
    ui_goto(UI_PAGE_GROUP);
}

static void create(lv_obj_t *parent)
{
    s_rows      = wfc_calloc(MEMBERS_MAX, sizeof(*s_rows));
    s_count     = 0;
    s_ask_names = false;
    s_ask_group = false;

    s_body = lv_obj_create(parent);
    ui_style_flat(s_body);
    lv_obj_set_width(s_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_style_pad_all(s_body, 6, 0);
    lv_obj_set_style_pad_row(s_body, 4, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);

    s_bar = lv_obj_create(parent);
    ui_style_flat(s_bar);
    lv_obj_set_size(s_bar, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_bar, lv_color_hex(UI_C_LINE), 0);
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_pad_all(s_bar, 6, 0);
    lv_obj_set_style_pad_column(s_bar, 6, 0);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);

    /* All three are built here, because what they say never changes. Whether
     * the middle one is SHOWN does change: on a board that has just opened a
     * group for the first time the roster is not cached yet, so "am I allowed
     * to remove people" reads false until the GPGM lands. Deciding it here
     * once would mean the button never appears until the page is reopened. */
    action("邀请", UI_C_ACCENT, add_clicked);
    s_kick = action("移出", UI_C_RAISED, kick_clicked);
    action("退出", UI_C_BAD, quit_clicked);
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_NAMES | UI_DIRTY_CONVS)) == 0) {
        return;
    }
    if (s_body == NULL) {
        return;
    }
    draw_body();

    /* Hidden rather than absent, so the other two keep their width instead of
     * jumping when the roster arrives. */
    if (i_can_remove()) {
        lv_obj_remove_flag(s_kick, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_kick, LV_OBJ_FLAG_HIDDEN);
    }
}

static void destroy(void)
{
    close_confirm();
    wfc_free(s_rows);
    s_rows      = NULL;
    s_body      = NULL;
    s_bar       = NULL;
    s_kick      = NULL;
    s_count     = 0;
    s_ask_names = false;
    s_ask_group = false;
}

static void title(char *buf, size_t buf_size)
{
    wfc_conversation_t conv = { .type = WFC_CONV_GROUP, .line = 0 };

    strlcpy(conv.target, s_group, sizeof(conv.target));
    wfc_get_conversation_title(&conv, buf, buf_size);
}

const ui_page_def_t ui_page_group = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
