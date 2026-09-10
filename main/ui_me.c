/* 我的 -- this account, and the two switches that are not a message.
 *
 * The nav bar had four tabs and two of them were instruments: 状态, which
 * read a dozen counters out of the client, and 日志, which held a ring of the
 * last thirty-two lines. Both were written for a board that was being brought
 * up, and both lost their job to the serial console -- ui_log() still writes
 * every one of those lines there, where a real investigation happens anyway
 * and where they cost no PSRAM and no repaint. What was left on the panel was
 * two tabs a user of a chat client has no reason to open.
 *
 * So this is the third tab every IM has instead: who you are signed in as, and
 * the settings that are about the board rather than about a conversation.
 *
 * The profile at the top is read the way every other page reads a profile --
 * out of the store at repaint time, asked for in prime() when it is not
 * cached -- so this account's own nickname fills itself in a moment after the
 * first sync, with no plumbing from the application. Which is also why there
 * is a monogram rather than a portrait: the panel has no image cache for
 * avatars, and a letter in a circle says the same thing for the price of one
 * label.
 *
 * The three rows under it are the ones the old 状态 page had that are worth a
 * glance from someone who is not debugging: what WiFi the board is on, what
 * address it was given, and whether the long link is up. They are also the
 * three the client cannot answer for itself, which is why they are pushed in
 * through ui.h and kept here beside the widgets -- they have to survive
 * leaving the page and coming back.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_timer.h"

#include "app_cfg.h"
#include "ui_page.h"
#include "wfc_store.h"

/* ------------------------------------------------------------ the account */

static lv_obj_t *s_monogram;
static lv_obj_t *s_name;
static lv_obj_t *s_account;

/* Set by refresh() when this account's own profile was not cached, drained by
 * prime() -- the arrangement ui_contact.c uses, and for the same reason: the
 * store's getter answers from the cache or says it cannot, and asking is what
 * happens one tick later, off the display lock.
 *
 * Armed only on a repaint that names could have changed, never on the
 * one-second tick that redraws everything else here. That distinction is the
 * whole of the rate limiting: a profile the server does not answer for would
 * otherwise become one UPUI per second for as long as the page is open. */
static bool s_ask_profile;

/* ------------------------------------------------------------- the board */

typedef enum {
    ROW_WIFI = 0,
    ROW_IP,
    ROW_LINK,
    ROW_COUNT,
} row_t;

static const char *const ROW_NAMES[ROW_COUNT] = {
    [ROW_WIFI] = "网络",
    [ROW_IP]   = "地址",
    [ROW_LINK] = "连接",
};

static lv_obj_t *s_value[ROW_COUNT];

/* Pushed in from the application; the UI task is the only writer. */
static char s_wifi[48]   = "-";
static char s_ip[24]     = "-";
static char s_uid[WFC_TARGET_MAX];

/* When the long link came up, so the page can say how long it has held. Kept
 * here because the client reports a status, not a duration. */
static int64_t s_link_up_ms;

void ui_me_set_wifi(const char *ssid, int rssi)
{
    if (ssid == NULL || ssid[0] == '\0') {
        strlcpy(s_wifi, "未连接", sizeof(s_wifi));
    } else {
        snprintf(s_wifi, sizeof(s_wifi), "%s  %d dBm", ssid, rssi);
    }
    ui_dirty(UI_DIRTY_STATUS);
}

void ui_me_set_ip(const char *ip)
{
    strlcpy(s_ip, ip != NULL && ip[0] != '\0' ? ip : "-", sizeof(s_ip));
    ui_dirty(UI_DIRTY_STATUS);
}

void ui_me_set_account(const char *user_id)
{
    strlcpy(s_uid, user_id != NULL ? user_id : "", sizeof(s_uid));
    ui_dirty(UI_DIRTY_STATUS);
}

/* The account this page is about. The application pushes it in before there
 * is a client -- it comes out of NVS at boot -- and the client answers for it
 * afterwards; either is the same string, and taking whichever exists is what
 * lets this page draw itself during a boot that has not got to a login yet. */
static const char *self_id(void)
{
    const char *uid = wfc_client_user_id();

    return uid != NULL && uid[0] != '\0' ? uid : s_uid;
}

/* ---------------------------------------------------------------- actions */

/* Clearing the network or the account cannot be undone from the panel -- the
 * board restarts into a QR code and needs a phone to get back -- so neither
 * happens on one tap. The first tap arms the button and relabels it, and it
 * disarms itself if the second tap does not come.
 *
 * A confirmation dialog would be the other way to do this, and it would cost
 * a modal, a backdrop and two more Chinese strings on a screen where the
 * finger is the only pointer; two taps says the same thing. */
#define CONFIRM_WINDOW_MS 5000

typedef struct {
    lv_obj_t   *label;
    const char *idle;
    uint32_t    colour;
    int64_t     armed_ms;
} action_t;

static action_t s_forget_wifi    = { .idle = "重新配网", .colour = UI_C_TEXT };
static action_t s_forget_account = { .idle = "退出登录", .colour = UI_C_BAD };

static void relabel(action_t *action, const char *text, uint32_t colour)
{
    lv_label_set_text(action->label, text);
    lv_obj_set_style_text_color(action->label, lv_color_hex(colour), 0);
}

/* Runs on the LVGL task with the display lock held, like every other button
 * callback here (ui_page.h), so the rule is the usual one: no blocking and no
 * calls into the client. An NVS write is neither -- it is a few milliseconds
 * of flash -- and what follows it is a restart, which ends the argument. */
static bool armed(action_t *action)
{
    int64_t now = esp_timer_get_time() / 1000;

    if (action->armed_ms != 0 && now - action->armed_ms < CONFIRM_WINDOW_MS) {
        return true;
    }

    action->armed_ms = now;
    relabel(action, "再按一次", UI_C_BAD);
    return false;
}

/* No wfc_client_disconnect() before either of these. It waits for the client's
 * own tasks, and waiting for anything from a button callback is what freezes
 * the panel; the server treats a board that vanishes the same way it treats
 * one that said goodbye. */
static void forget_wifi(lv_event_t *e)
{
    (void)e;

    if (!armed(&s_forget_wifi)) {
        return;
    }
    app_cfg_clear_wifi();
    esp_restart();
}

static void forget_account(lv_event_t *e)
{
    (void)e;

    if (!armed(&s_forget_account)) {
        return;
    }
    app_cfg_clear_account();
    esp_restart();
}

/* Called from the repaint, which is once a second here: a button that was
 * armed and then left alone goes back to saying what it does. */
static void disarm_stale(action_t *action)
{
    if (action->label == NULL || action->armed_ms == 0) {
        return;
    }
    if (esp_timer_get_time() / 1000 - action->armed_ms < CONFIRM_WINDOW_MS) {
        return;
    }

    action->armed_ms = 0;
    relabel(action, action->idle, action->colour);
}

/* ---------------------------------------------------------------- drawing */

/* A rounded panel to group rows in, which is what a settings page on a phone
 * is made of. */
static lv_obj_t *card(lv_obj_t *parent, int32_t pad_row)
{
    lv_obj_t *obj = lv_obj_create(parent);

    ui_style_flat(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_pad_all(obj, 8, 0);
    lv_obj_set_style_pad_row(obj, pad_row, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    return obj;
}

/* Every label on this page is written on a tick that mostly changes nothing,
 * and lv_label_set_text() copies the string and invalidates the object
 * whether or not it differs. So ask first: a page that says the same thing it
 * said a second ago should cost no repaint at all. */
static void say(lv_obj_t *label, const char *text)
{
    if (label != NULL && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set(row_t row, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set(row_t row, const char *fmt, ...)
{
    char    buf[64];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    say(s_value[row], buf);
}

/* One UTF-8 character out of `name`, for the circle. The lead byte says how
 * many bytes follow it, and a name this board could not resolve is drawn as
 * <uid> (wfc_get_display_name), whose first character is punctuation -- so
 * the brackets are stepped over rather than framed. */
static void monogram(const char *name, char *buf, size_t buf_size)
{
    const char *p = name;
    size_t      n;

    while (*p == '<') {
        p++;
    }
    if (*p == '\0') {
        strlcpy(buf, "?", buf_size);
        return;
    }

    n = (*p & 0x80) == 0x00 ? 1
      : (*p & 0xE0) == 0xC0 ? 2
      : (*p & 0xF0) == 0xE0 ? 3
                            : 4;
    if (n >= buf_size) {
        n = buf_size - 1;
    }
    memcpy(buf, p, n);
    buf[n] = '\0';
}

/* `may_ask` is what arms prime(); see s_ask_profile. */
static void draw_account(bool may_ask)
{
    const char     *uid    = self_id();
    wfc_user_info_t me     = { 0 };
    bool            cached;
    char            name[WFC_NAME_MAX];
    char            line[WFC_NAME_MAX + 8];
    char            initial[5];

    /* Before there is an account at all -- a board that has not been through
     * 扫码登录 yet. It cannot navigate here, because a setup page hides the
     * nav bar, but it can be drawn here by the repaint that happens while one
     * is still on screen. */
    if (uid[0] == '\0') {
        say(s_monogram, "?");
        say(s_name, "未登录");
        say(s_account, "");
        return;
    }

    cached = wfc_store_get_user(uid, &me);

    /* The same call every other page names a person with, pointed at this
     * account: the nickname if there is one, the login name if that is all
     * there is, and <uid> until the first sync answers. */
    wfc_get_display_name(uid, NULL, name, sizeof(name));

    monogram(name, initial, sizeof(initial));
    say(s_monogram, initial);
    say(s_name, name);

    /* 账号 is the login name, which is what a phone shows under the nickname
     * and what somebody would type to find you. Without a profile there is
     * only the ID, and saying which of the two is on screen is the difference
     * between a page that is still loading and one that is wrong. */
    if (me.name[0] != '\0') {
        snprintf(line, sizeof(line), "账号 %s", me.name);
    } else {
        snprintf(line, sizeof(line), "ID %s", uid);
    }
    say(s_account, line);

    if (may_ask) {
        s_ask_profile = !cached;
    }
}

static void build_account(lv_obj_t *parent)
{
    lv_obj_t *box = card(parent, 0);

    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, 8, 0);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *circle = lv_obj_create(box);
    ui_style_flat(circle);
    lv_obj_set_size(circle, 40, 40);
    lv_obj_set_style_bg_color(circle, lv_color_hex(UI_C_ACCENT), 0);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);

    s_monogram = lv_label_create(circle);
    lv_label_set_text(s_monogram, "?");
    lv_obj_set_style_text_color(s_monogram, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_monogram);

    lv_obj_t *column = ui_column(box, 2);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_grow(column, 1);
    lv_obj_set_width(column, 0);   /* grow decides it; 100% would overflow */

    s_name = lv_label_create(column);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_name, LV_PCT(100));
    lv_label_set_text(s_name, "-");
    lv_obj_set_style_text_color(s_name, lv_color_hex(UI_C_TEXT), 0);

    s_account = lv_label_create(column);
    lv_label_set_long_mode(s_account, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_account, LV_PCT(100));
    lv_label_set_text(s_account, "");
    lv_obj_set_style_text_color(s_account, lv_color_hex(UI_C_DIM), 0);
}

static void build_board(lv_obj_t *parent)
{
    lv_obj_t *box = card(parent, 4);

    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(box);

        ui_style_flat(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 8, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_width(name, 60);
        lv_label_set_text(name, ROW_NAMES[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_C_DIM), 0);

        lv_obj_t *value = lv_label_create(row);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(value, 1);
        lv_label_set_text(value, "-");
        lv_obj_set_style_text_color(value, lv_color_hex(UI_C_TEXT), 0);

        s_value[i] = value;
    }
}

static void build_action(lv_obj_t *bar, action_t *action, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(bar);

    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    action->label    = lv_label_create(btn);
    action->armed_ms = 0;
    lv_label_set_text(action->label, action->idle);
    lv_obj_set_style_text_color(action->label, lv_color_hex(action->colour), 0);
    lv_obj_center(action->label);
}

/* ------------------------------------------------------------- the page */

static void create(lv_obj_t *parent)
{
    lv_obj_t *body = lv_obj_create(parent);

    ui_style_flat(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    build_account(body);
    build_board(body);

    /* The two switches sit outside the scrolling area, the way the contact
     * page keeps its own actions there: two cards and a 240 px screen is
     * already a scroll on a board whose name is long, and a button that has
     * to be found by flicking is a button somebody will conclude is gone. */
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

    build_action(bar, &s_forget_wifi, forget_wifi);
    build_action(bar, &s_forget_account, forget_account);
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_STATUS | UI_DIRTY_NAMES)) == 0 || s_name == NULL) {
        return;
    }

    draw_account((dirty & UI_DIRTY_NAMES) != 0);

    wfc_connection_status_t status = wfc_client_status();
    bool up = status == WFC_STATUS_CONNECTED || status == WFC_STATUS_RECEIVING;

    if (up && s_link_up_ms == 0) {
        s_link_up_ms = esp_timer_get_time() / 1000;
    } else if (!up) {
        s_link_up_ms = 0;
    }

    set(ROW_WIFI, "%s", s_wifi);
    set(ROW_IP, "%s", s_ip);

    if (s_link_up_ms != 0) {
        unsigned held = (unsigned)((esp_timer_get_time() / 1000 - s_link_up_ms) / 1000);

        set(ROW_LINK, "%s %02u:%02u:%02u", wfc_is_syncing() ? "同步中" : "已连接",
            held / 3600, held / 60 % 60, held % 60);
    } else {
        set(ROW_LINK, "%s", wfc_status_str(status));
    }

    disarm_stale(&s_forget_wifi);
    disarm_stale(&s_forget_account);
}

/* Off both locks, so this is where the one thing this page can ask the server
 * for goes: this account's own profile, while it is missing and only on the
 * repaints that arm it (s_ask_profile). For this account that means once,
 * shortly after the first connection -- wfc_get_user_info() is a cache lookup
 * that misses and a UPUI the client dedups, and the answer comes back as a
 * user-infos event, which redraws the name above. */
static void prime(void)
{
    if (!s_ask_profile) {
        return;
    }
    s_ask_profile = false;

    wfc_user_info_t me;

    wfc_get_user_info(self_id(), false, &me);
}

static void destroy(void)
{
    memset(s_value, 0, sizeof(s_value));
    s_monogram    = NULL;
    s_name        = NULL;
    s_account     = NULL;
    s_ask_profile = false;

    s_forget_wifi.label       = NULL;
    s_forget_wifi.armed_ms    = 0;
    s_forget_account.label    = NULL;
    s_forget_account.armed_ms = 0;
}

static void title(char *buf, size_t buf_size)
{
    strlcpy(buf, "我的", buf_size);
}

const ui_page_def_t ui_page_me = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = true,
};
