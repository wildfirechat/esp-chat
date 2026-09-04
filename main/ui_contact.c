/* 一个联系人 -- who they are, and what can be done about them.
 *
 * Pushed from 联系人 (ui_contacts.c) and handed nothing but a user ID, which
 * is the whole of its state. Everything drawn is read back out of the store
 * at repaint time, so this page has no way of showing something the rest of
 * the client disagrees with, and a profile arriving while it is open redraws
 * it with no help from whoever opened it.
 *
 * The reads here are the store's own -- wfc_store_get_user() and
 * wfc_store_get_friend() rather than wfc_get_user_info() -- for the reason
 * ui_page.h gives: refresh() runs under the display lock, and the client's
 * getter sends a UPUI on a cache miss, which ends in a blocking send() on the
 * long link. The store's getter answers from the cache or says it cannot, and
 * asking is prime()'s job, one tick later and off the lock.
 *
 * Which is also why a profile that is not cached is a state this page draws
 * rather than a state it waits in: the ID and the relationship come from
 * tables that are always there, the name and the account come from one that
 * may not be yet, and the missing half fills itself in a moment later.
 */

#include <stdio.h>
#include <string.h>

#include "ui_page.h"
#include "wfc_store.h"

/* The person this page is about. The one piece of state, set before the
 * navigation that builds the widgets. */
static char s_uid[WFC_TARGET_MAX];

static lv_obj_t *s_body;        /* the facts; rebuilt whole on every redraw */
static lv_obj_t *s_call;
static lv_obj_t *s_call_label;
/* The relationship row. Its contents depend on whether this person is a
 * friend, which changes while the page is up -- a request being accepted, a
 * deletion being acknowledged -- so it is emptied and refilled on every
 * redraw like the facts above it, not built once. */
static lv_obj_t *s_rel_bar;

/* What was asked for, parked by a button or by the composer and drained by
 * prime(). The three relationship writes are the reason this page has a
 * prime() that does more than fetch a profile. */
typedef enum {
    WANT_NOTHING = 0,
    WANT_ADD,      /* FAR, with s_text as the reason */
    WANT_DELETE,   /* FDL */
    WANT_ALIAS,    /* FALS, with s_text as the alias */
} want_t;

static want_t s_want;
/* Which write the composer is collecting text for. Separate from s_want, and
 * the separation is not cosmetic: s_want is what prime() acts on, and prime()
 * runs on the UI task on the next tick after ANY repaint -- including one
 * that happens between the button being pressed and the navigation to the
 * composer being dequeued. Setting s_want in the button would therefore send
 * a friend request with no reason in it, sometimes. It is set in the
 * composer's callback instead, where this page is not the current one and its
 * prime() cannot run until the navigation back has already happened. */
static want_t s_collecting;
static char   s_text[WFC_NAME_MAX];

/* Set by refresh() when the profile was not cached, drained by prime() --
 * the same arrangement ui_contacts.c uses, and for the same reason: prime()
 * runs about once a second whatever happens, and a profile the server will
 * not answer for must not become a UPUI a second. */
static bool s_ask_profile;

/* --------------------------------------------------------------- drawing */

/* One "名字   值" line. Empty values are not drawn at all rather than drawn
 * as a dash: a profile this board has never seen has three of them, and three
 * dashes look like a broken page instead of an unsynced one. */
static void field(const char *name, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return;
    }

    lv_obj_t *row = lv_obj_create(s_body);

    ui_style_flat(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, 60);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_C_DIM), 0);

    lv_obj_t *text = lv_label_create(row);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_flex_grow(text, 1);
    lv_label_set_text(text, value);
    lv_obj_set_style_text_color(text, lv_color_hex(UI_C_TEXT), 0);
}

static void draw_facts(void)
{
    wfc_user_info_t user   = { 0 };
    wfc_friend_t    entry  = { 0 };
    bool            cached = wfc_store_get_user(s_uid, &user);
    char            name[WFC_NAME_MAX];

    /* True for a relationship that ended too -- the row is kept so the next FP
     * does not re-deliver it (wfc_store.h) -- so the alias is worth reading
     * either way and the state is not. That question is wfc_is_friend()'s. */
    bool known = wfc_store_get_friend(s_uid, &entry);

    lv_obj_clean(s_body);

    wfc_get_display_name(s_uid, NULL, name, sizeof(name));

    lv_obj_t *head = ui_label(s_body, name, UI_C_TEXT);
    lv_obj_set_style_pad_bottom(head, 6, 0);

    if (known && entry.alias[0] != '\0') {
        /* The header is already showing the alias -- it wins over every other
         * name -- so this row is what they call themselves. */
        field("备注", entry.alias);
    }
    field("名字", user.display_name);
    field("账号", user.name);
    if (user.gender == 1 || user.gender == 2) {
        field("性别", user.gender == 1 ? "男" : "女");
    }
    field("ID", s_uid);

    if (wfc_is_friend(s_uid)) {
        field("关系", entry.blacked != 0 ? "好友（已屏蔽）" : "好友");
    } else {
        field("关系", known ? "已经不是好友了" : "不是好友");
    }

    if (!cached) {
        lv_obj_t *note = ui_label(s_body, "资料还没同步过来", UI_C_DIM);

        lv_obj_set_style_pad_top(note, 6, 0);
        lv_obj_set_style_text_font(note, UI_FONT_SMALL, 0);
    }
    s_ask_profile = !cached;
}

/* ---------------------------------------------------------------- actions */

/* All of these run on the LVGL task with the display lock held, so none of
 * them may do more than post or park -- ui_page.h's rule, and the reason the
 * three relationship writes go through s_want and prime() rather than calling
 * the client from here. ui_chat_open() sets a conversation and queues a
 * navigation; ui_call_dial() returns before the invite is sent. */

static void message_clicked(lv_event_t *e)
{
    (void)e;

    wfc_conversation_t conv = { .type = WFC_CONV_SINGLE, .line = 0 };

    strlcpy(conv.target, s_uid, sizeof(conv.target));
    ui_chat_open(&conv);
}

static void call_clicked(lv_event_t *e)
{
    (void)e;
    ui_call_dial(s_uid);
}

/* Two things want a line of text -- the reason on a friend request and the
 * alias on a friend -- and the composer is the page that collects one
 * (ui_page.h). Which of them is being written is in s_collecting when its
 * callback fires, so one callback does for both. */
static void text_entered(const char *text)
{
    wfc_copy_text(s_text, sizeof(s_text), text != NULL ? text : "");
    s_want = s_collecting;
}

static void add_clicked(lv_event_t *e)
{
    (void)e;

    s_collecting = WANT_ADD;
    s_text[0]    = '\0';
    ui_compose_open("打个招呼", text_entered);
}

static void alias_clicked(lv_event_t *e)
{
    (void)e;

    s_collecting = WANT_ALIAS;
    s_text[0]    = '\0';
    ui_compose_open("设置备注", text_entered);
}

/* Deleting is the one action here that cannot be undone from this page and
 * that changes what the OTHER end sees -- the server clears the relationship
 * on both accounts -- so it asks first, the way leaving a group does. */
static lv_obj_t *s_confirm;

static void close_confirm(void)
{
    if (s_confirm != NULL) {
        lv_obj_delete(s_confirm);
        s_confirm = NULL;
    }
}

static void confirm_delete(lv_event_t *e)
{
    (void)e;
    close_confirm();
    s_want = WANT_DELETE;
    ui_dirty(UI_DIRTY_FRIENDS);
}

static void confirm_dismissed(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        close_confirm();
    }
}

static void delete_clicked(lv_event_t *e)
{
    (void)e;
    close_confirm();

    /* On lv_layer_top(), so that this page repainting underneath -- which it
     * does whenever a profile lands -- does not delete the card mid-press.
     * Same reasoning as ui_convs.c's long-press menu. */
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

    ui_label(card, "删除之后对方那边也不再是好友。", UI_C_DIM);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_BAD), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_add_event_cb(btn, confirm_delete, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "删除好友");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
}

static lv_obj_t *action(lv_obj_t *bar, const char *text, uint32_t colour,
                        lv_event_cb_t on_click, lv_obj_t **label)
{
    lv_obj_t *btn = lv_button_create(bar);

    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_hex(colour), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *text_label = lv_label_create(btn);
    lv_obj_set_style_text_color(text_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(text_label, text);
    lv_obj_center(text_label);

    if (label != NULL) {
        *label = text_label;
    }
    return btn;
}

/* ------------------------------------------------------------ the page */

void ui_contact_open(const char *user_id)
{
    if (user_id == NULL || user_id[0] == '\0') {
        return;
    }
    /* Anything parked for the previous person does not follow us to this one
     * -- the same rule ui_chat_open() has about the composer's text. Not
     * cleared in create(), because create() is also what runs on the way BACK
     * from the composer, and that is exactly when the parked text matters. */
    s_want       = WANT_NOTHING;
    s_collecting = WANT_NOTHING;
    s_text[0]    = '\0';
    /* Copied rather than pointed at: the row that was tapped belongs to the
     * list page, and the list page is deleted before this one is built. */
    strlcpy(s_uid, user_id, sizeof(s_uid));
    ui_goto(UI_PAGE_CONTACT);
}

static void create(lv_obj_t *parent)
{
    s_ask_profile = false;

    s_body = lv_obj_create(parent);
    ui_style_flat(s_body);
    lv_obj_set_width(s_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_style_pad_all(s_body, 8, 0);
    lv_obj_set_style_pad_row(s_body, 3, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);

    /* The actions sit outside the scrolling area: the facts above them can
     * grow past a screenful, and the two buttons are the reason the page is
     * open. */
    lv_obj_t *bar = lv_obj_create(parent);
    ui_style_flat(bar);
    lv_obj_set_size(bar, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_C_LINE), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    /* Sending works offline in the sense that matters here: the chat page
     * opens, shows what the store holds, and says so in its own input bar.
     * A call cannot, so that one is the button that greys out. */
    action(bar, "发消息", UI_C_ACCENT, message_clicked, NULL);

    /* Same rule as the chat page's phone button: with CONFIG_APP_CALL=n there
     * is nothing behind it, so there is no button (ui_page.h). */
    if (ui_call_available()) {
        s_call = action(bar, "语音通话", UI_C_OK, call_clicked, &s_call_label);
    }

    /* A second row for the writes, because two rows of two is what a 320 px
     * bar holds and because these change: whether this person is a friend is
     * the question the row answers, and the answer moves while the page is up
     * -- a request being accepted, a deletion being acknowledged. So the row
     * is built empty here and filled by draw_relationship() on every redraw,
     * exactly like the facts above it. */
    s_rel_bar = lv_obj_create(parent);
    ui_style_flat(s_rel_bar);
    lv_obj_set_size(s_rel_bar, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(s_rel_bar, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_pad_hor(s_rel_bar, 6, 0);
    lv_obj_set_style_pad_bottom(s_rel_bar, 6, 0);
    lv_obj_set_style_pad_column(s_rel_bar, 6, 0);
    lv_obj_set_flex_flow(s_rel_bar, LV_FLEX_FLOW_ROW);
}

/* Setting an alias and deleting both need a friend, so they replace the one
 * button that does not. Nothing at all for this account's own page: WFC has
 * no notion of being your own friend, and the server would refuse. */
static void draw_relationship(void)
{
    lv_obj_clean(s_rel_bar);

    if (strcmp(s_uid, wfc_client_user_id()) == 0) {
        return;
    }
    if (wfc_is_friend(s_uid)) {
        action(s_rel_bar, "设置备注", UI_C_RAISED, alias_clicked, NULL);
        action(s_rel_bar, "删除好友", UI_C_BAD, delete_clicked, NULL);
    } else {
        action(s_rel_bar, "加好友", UI_C_OK, add_clicked, NULL);
    }
}

static void refresh(uint32_t dirty)
{
    if (s_body == NULL) {
        return;
    }

    /* Two masks, because the two halves of this page move at different
     * speeds: the facts change when a profile or the friend list lands, and
     * the call button changes with the link and with the call itself -- and
     * UI_DIRTY_STATUS arrives once a second, which is not a rate to rebuild
     * a dozen labels at. */
    if ((dirty & (UI_DIRTY_NAMES | UI_DIRTY_FRIENDS)) != 0) {
        draw_facts();
        draw_relationship();
    }

    if (s_call != NULL &&
        (dirty & (UI_DIRTY_STATUS | UI_DIRTY_CALL | UI_DIRTY_NAMES)) != 0) {
        bool busy      = ui_call_busy();
        bool connected = wfc_client_status() == WFC_STATUS_CONNECTED ||
                         wfc_client_status() == WFC_STATUS_RECEIVING;

        /* A call already up turns this into the way back to it, which is what
         * makes the call page's back arrow a minimise rather than a hang-up.
         * ui_call_dial() is already both; the label is what says so. */
        lv_label_set_text(s_call_label, busy ? "回到通话" : "语音通话");
        if (connected || busy) {
            lv_obj_remove_state(s_call, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_call, LV_STATE_DISABLED);
        }
    }
}

/* Off both locks, so this is where the three writes go out. They are sent
 * before the profile lookup for the reason every other page sends first: an
 * action the user just asked for should not be queued behind a fetch.
 *
 * Nothing is drawn as done here. The client files the row when the server
 * agrees and raises the friend-list event, which repaints this page with the
 * new relationship on it -- so a refusal leaves the page saying what is
 * actually true (wfc_client.h). */
static void prime(void)
{
    want_t want = s_want;

    s_want = WANT_NOTHING;

    if (want != WANT_NOTHING) {
        esp_err_t   err;
        const char *what;

        switch (want) {
        case WANT_ADD:
            err  = wfc_send_friend_request(s_uid, s_text, NULL, NULL);
            what = "好友请求";
            break;
        case WANT_DELETE:
            err  = wfc_delete_friend(s_uid, NULL, NULL);
            what = "删除好友";
            break;
        default:
            err  = wfc_set_friend_alias(s_uid, s_text, NULL, NULL);
            what = "备注";
            break;
        }
        if (err != ESP_OK) {
            ui_logf(UI_LOG_ERROR, "%s未能提交: %s", what, esp_err_to_name(err));
        }
    }

    if (!s_ask_profile) {
        return;
    }
    s_ask_profile = false;

    wfc_user_info_t user;

    wfc_get_user_info(s_uid, false, &user);
}

static void destroy(void)
{
    close_confirm();
    s_body        = NULL;
    s_call        = NULL;
    s_call_label  = NULL;
    s_rel_bar     = NULL;
    s_ask_profile = false;
}

static void title(char *buf, size_t buf_size)
{
    wfc_get_display_name(s_uid, NULL, buf, buf_size);
}

const ui_page_def_t ui_page_contact = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
