/* One call, on a 320x240 panel.
 *
 * The page is a function of one thing -- the session's state (wfav_session.h)
 * -- and every repaint reads it fresh. That is the same rule the chat page
 * follows against the store, and it is what makes the hard part easy: a call
 * changes state from four different directions (the far end answered, the far
 * end hung up, the room went away, the ring timed out) and none of them has
 * to know a screen exists. They move the state; the screen is redrawn; the
 * buttons are whatever that state should offer.
 *
 *   INCOMING              拒绝  接听
 *   OUTGOING/CONNECTING   挂断
 *   CONNECTED             静音  挂断, and a running timer
 *   IDLE                  why it ended, then the page closes itself
 *
 * The timer is why this page repaints on UI_DIRTY_STATUS as well: the shell
 * already sets that bit once a second for the clock in the header, so a
 * running call costs no timer of its own.
 *
 * ------------------------------------------------------------------------
 * What this page deliberately does not do.
 *
 * It does not call into wfav from a button callback and then wait. Answer,
 * hangup and mute are queue posts that return immediately, which they have to
 * be: a button runs on the LVGL task with the display lock held, and
 * answering a call sends an IM message and then talks to a TURN server.
 *
 * It also holds nothing about the call between repaints. There is no session
 * handle to hold (wfav_session.h explains why), so every value is read fresh
 * and a call that ends mid-frame reads as IDLE rather than as a dangling
 * pointer.
 *
 * ------------------------------------------------------------------------
 * This is also the only file in main/ that includes wfav.h, and that is on
 * purpose rather than by accident. Calls are optional (CONFIG_APP_CALL), and
 * ui_call_none.c is the other half of that choice: the same functions from
 * ui_page.h, doing nothing, for a build with no AV SDK beside it. Keeping the
 * SDK behind this one file is what lets the two exist without a single #ifdef
 * anywhere in the shell, the chat page or app_main.c.
 *
 * So everything the rest of the application does with a call -- start the
 * engine, subscribe, ask whether one is up, place one -- ends up here, below
 * the page it belongs to.
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "ui_page.h"
#include "wfav.h"

static const char *TAG = "ui_call";

/* How long the ended state stays up before the page closes itself. Long
 * enough to read "对方已挂断", short enough that nobody wonders whether the
 * board is stuck. */
#define LINGER_MS 2000

static lv_obj_t *s_who;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_buttons;
static lv_obj_t *s_left;        /* 拒绝 / 静音; hidden when there is one button */
static lv_obj_t *s_left_label;
static lv_obj_t *s_right;       /* 接听 / 挂断 */
static lv_obj_t *s_right_label;

/* The name is resolved once per repaint from the profile cache, and the ID it
 * falls back to is kept so prime() can ask for the profile. */
static char s_peer_id[WFC_TARGET_MAX];
static bool s_peer_wanted;

/* When the call ended, so the page can close itself a moment later. 0 while a
 * call is up. */
static uint32_t s_ended_at;

/* The ended state's wording comes from the event rather than from the session,
 * because the session is IDLE by the time anything can read it and IDLE does
 * not say why. on_call_ended() below sends it round through the shell's queue
 * and the UI task parks it here, which is also why it outlives destroy(): the
 * page is gone by then and the title still has to be right for the frame that
 * closes it. */
static char s_ended_text[32];

/* ------------------------------------------------------------- buttons */

static void left_clicked(lv_event_t *e)
{
    (void)e;

    switch (wfav_session_state()) {
    case WFAV_STATE_IDLE:
        break;
    case WFAV_STATE_INCOMING:
        wfav_hangup();   /* reject: the same message, a different reason */
        break;
    default:
        wfav_set_audio_muted(!wfav_session_audio_muted());
        break;
    }
}

static void right_clicked(lv_event_t *e)
{
    (void)e;

    switch (wfav_session_state()) {
    case WFAV_STATE_IDLE:
        ui_back();
        break;
    case WFAV_STATE_INCOMING:
        wfav_answer();
        break;
    default:
        wfav_hangup();
        break;
    }
}

/* A call's buttons are large and far apart on purpose: this is the one screen
 * where hitting the wrong one is expensive, and it is operated with a
 * fingertip by someone who is already talking. */
static lv_obj_t *make_button(lv_obj_t *parent, uint32_t colour, lv_obj_t **label,
                             lv_event_cb_t on_click)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, 108, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(colour), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);

    *label = lv_label_create(btn);
    lv_obj_set_style_text_color(*label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(*label);
    return btn;
}

/* ------------------------------------------------------------- the page */

void ui_call_open(void)
{
    ui_goto(UI_PAGE_CALL);
}

static void create(lv_obj_t *parent)
{
    s_ended_at    = 0;
    s_peer_wanted = false;
    s_peer_id[0]  = '\0';

    lv_obj_t *body = lv_obj_create(parent);

    ui_style_flat(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_style_pad_all(body, 10, 0);

    s_who = lv_label_create(body);
    lv_label_set_long_mode(s_who, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_who, LV_PCT(90));
    lv_obj_set_style_text_align(s_who, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_who, lv_color_hex(UI_C_TEXT), 0);
    lv_label_set_text(s_who, "");

    /* The line that carries the state, and once connected the timer. It is
     * the biggest thing on the screen because it is the only thing on it that
     * changes. */
    s_status = lv_label_create(body);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_C_ACCENT), 0);
    lv_label_set_text(s_status, "");

    s_hint = lv_label_create(body);
    lv_obj_set_style_text_font(s_hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_C_DIM), 0);
    lv_label_set_text(s_hint, "");

    s_buttons = lv_obj_create(parent);
    ui_style_flat(s_buttons);
    lv_obj_set_size(s_buttons, LV_PCT(100), 62);
    lv_obj_set_flex_flow(s_buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_buttons, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_left  = make_button(s_buttons, UI_C_RAISED, &s_left_label, left_clicked);
    s_right = make_button(s_buttons, UI_C_BAD, &s_right_label, right_clicked);
}

/* Fetching the peer's profile is the same deal every other page makes: the
 * name is drawn from the cache, a miss is drawn as the ID in angle brackets,
 * and asking happens here, off the display lock. */
static void prime(void)
{
    if (s_peer_wanted && s_peer_id[0] != '\0') {
        wfc_user_info_t user;

        wfc_get_user_info(s_peer_id, false, &user);
        s_peer_wanted = false;
    }
}

static void set_buttons(bool two, uint32_t left_colour, const char *left_text,
                        uint32_t right_colour, const char *right_text)
{
    if (two) {
        lv_obj_remove_flag(s_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_left, lv_color_hex(left_colour), 0);
        lv_label_set_text(s_left_label, left_text);
    } else {
        lv_obj_add_flag(s_left, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(s_right, lv_color_hex(right_colour), 0);
    lv_label_set_text(s_right_label, right_text);
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_CALL | UI_DIRTY_STATUS | UI_DIRTY_NAMES)) == 0) {
        return;
    }
    if (s_status == NULL) {
        return;
    }

    wfav_call_state_t state = wfav_session_state();

    /* Who we are talking to. On an outgoing call this is set before the room
     * exists; on an incoming one it is the caller. */
    char peer[WFC_TARGET_MAX] = "";

    if (wfav_session_peer(peer, sizeof(peer))) {
        if (strcmp(peer, s_peer_id) != 0) {
            strlcpy(s_peer_id, peer, sizeof(s_peer_id));
            s_peer_wanted = true;
        }
    }
    if (s_peer_id[0] != '\0') {
        char name[WFC_NAME_MAX];

        wfc_get_display_name(s_peer_id, NULL, name, sizeof(name));
        lv_label_set_text(s_who, name);
    }

    switch (state) {
    case WFAV_STATE_INCOMING:
        lv_label_set_text(s_status, "邀请你语音通话");
        lv_label_set_text(s_hint, "");
        set_buttons(true, UI_C_RAISED, "拒绝", UI_C_OK, "接听");
        break;

    case WFAV_STATE_OUTGOING:
        lv_label_set_text(s_status, "正在呼叫");
        lv_label_set_text(s_hint, "等待对方接听");
        set_buttons(false, 0, NULL, UI_C_BAD, "挂断");
        break;

    case WFAV_STATE_CONNECTING:
        lv_label_set_text(s_status, "接通中");
        lv_label_set_text(s_hint, "正在建立音频连接");
        set_buttons(false, 0, NULL, UI_C_BAD, "挂断");
        break;

    case WFAV_STATE_CONNECTED: {
        int64_t ms   = wfav_session_duration_ms();
        int     secs = (int)(ms / 1000);
        char    timer[16];

        snprintf(timer, sizeof(timer), "%02d:%02d", secs / 60, secs % 60);
        lv_label_set_text(s_status, timer);

        bool muted = wfav_session_audio_muted();

        lv_label_set_text(s_hint, muted ? "麦克风已关闭" : "");
        set_buttons(true, muted ? UI_C_ACCENT : UI_C_RAISED,
                    muted ? "取消静音" : "静音", UI_C_BAD, "挂断");
        break;
    }

    case WFAV_STATE_IDLE:
    default:
        /* Either the call ended while this page was up, or the page was
         * opened with nothing to show. Either way it closes itself; the
         * linger is so the reason can be read. */
        if (s_ended_at == 0) {
            s_ended_at = lv_tick_get();
        }
        lv_label_set_text(s_hint, "");
        set_buttons(false, 0, NULL, UI_C_RAISED, "返回");
        if (lv_tick_elaps(s_ended_at) > LINGER_MS) {
            ui_back();
        }
        break;
    }
}

void ui_call_set_end_reason(const char *text)
{
    strlcpy(s_ended_text, text != NULL ? text : "", sizeof(s_ended_text));
}

static void destroy(void)
{
    s_who         = NULL;
    s_status      = NULL;
    s_hint        = NULL;
    s_buttons     = NULL;
    s_left        = NULL;
    s_left_label  = NULL;
    s_right       = NULL;
    s_right_label = NULL;
    s_ended_at    = 0;
}

static void title(char *buf, size_t buf_size)
{
    if (!wfav_is_busy() && s_ended_text[0] != '\0') {
        strlcpy(buf, s_ended_text, buf_size);
        return;
    }
    strlcpy(buf, "语音通话", buf_size);
}

const ui_page_def_t ui_page_call = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    /* Not a home page: it is pushed on top of whatever was showing, and the
     * back arrow minimises it -- the call keeps running and the 通话 button
     * on the conversation brings it back. */
    .home = false,
};

/* ------------------------------------------------------- the AV SDK
 *
 * The events, first. They run on the AV task, so the same rules apply as to
 * the client's events in ui.c: set bits, append a log line, post -- never
 * touch a widget.
 *
 * The one that does more than that is the invite, and it is the reason this
 * page exists at all: nothing on the panel asked for it, so the screen has to
 * be brought up by the event itself. ui_call_open() is a queue post, so the
 * page switch still happens on the UI task like every other one. */

static void on_call_started(const char *call_id, bool incoming, void *ud)
{
    (void)call_id;
    (void)ud;

    ui_logf(UI_LOG_NOTE, incoming ? "收到语音通话邀请" : "正在呼叫");
    ui_dirty(UI_DIRTY_CALL);
    ui_call_open();
}

static void on_call_state(wfav_call_state_t state, void *ud)
{
    (void)state;
    (void)ud;
    ui_dirty(UI_DIRTY_CALL);
}

static void on_call_ended(wfav_end_reason_t reason, int64_t duration_ms, void *ud)
{
    (void)ud;

    const char *why = wfav_end_reason_str(reason);

    if (duration_ms > 0) {
        ui_logf(UI_LOG_NOTE, "%s，通话 %d 秒", why, (int)(duration_ms / 1000));
    } else {
        ui_log(UI_LOG_NOTE, why);
    }
    /* Through the shell's queue rather than straight into s_ended_text: the
     * page belongs to the UI task, and this is the AV task. */
    ui_call_post_end_reason(why);
    ui_dirty(UI_DIRTY_CALL | UI_DIRTY_CONVS | UI_DIRTY_MESSAGES);
}

static void on_call_participant(const char *user_id, void *ud)
{
    (void)ud;

    char who[WFC_NAME_MAX];

    wfc_get_display_name(user_id, NULL, who, sizeof(who));
    ESP_LOGI(TAG, "%s connected", who);
    ui_dirty(UI_DIRTY_CALL);
}

static void on_call_mute(const char *user_id, bool audio_muted, void *ud)
{
    (void)user_id;
    (void)audio_muted;
    (void)ud;
    ui_dirty(UI_DIRTY_CALL);
}

static void on_call_error(wfav_error_domain_t domain, int code,
                          const char *detail, void *ud)
{
    (void)ud;
    ui_logf(UI_LOG_ERROR, "通话出错 %s%d: %s",
            domain == WFAV_ERR_ROOM ? "(房间) " : "", code,
            detail != NULL ? detail : "");
}

/* --------------------------------------------------- what the app calls */

bool ui_call_available(void)
{
    return true;
}

void ui_call_subscribe(void)
{
    wfav_on_call_started(on_call_started, NULL);
    wfav_on_state_changed(on_call_state, NULL);
    wfav_on_call_ended(on_call_ended, NULL);
    wfav_on_participant_connected(on_call_participant, NULL);
    wfav_on_mute_changed(on_call_mute, NULL);
    wfav_on_error(on_call_error, NULL);
}

void ui_call_start(void)
{
    wfav_engine_config_t cfg;

    wfav_engine_default_config(&cfg);

    /* The defaults the SDK just filled in came from its own Kconfig, and a
     * build against a packaged libwfav.a cannot change those: they were
     * compiled in when the archive was made. TURN is deployment configuration
     * rather than SDK configuration -- which deployment this board belongs to
     * is the same question the baked IM-server licence answers -- so the
     * application gets
     * the last word here. Empty (the default) leaves the SDK's own setting
     * alone, which is what a board on the public deployment wants. */
    if (strlen(CONFIG_APP_TURN_URL) > 0) {
        cfg.ice_servers[0].url      = CONFIG_APP_TURN_URL;
        cfg.ice_servers[0].username = CONFIG_APP_TURN_USER;
        cfg.ice_servers[0].password = CONFIG_APP_TURN_PASSWORD;
        cfg.n_ice_servers           = 1;
        ESP_LOGI(TAG, "TURN: %s (from CONFIG_APP_TURN_URL)", CONFIG_APP_TURN_URL);
    }

    esp_err_t err = wfav_engine_start(&cfg);

    if (err != ESP_OK) {
        /* Not fatal -- see ui_page.h. Messaging works without calls. */
        ESP_LOGE(TAG, "the AV engine did not start: %s -- calls are unavailable",
                 esp_err_to_name(err));
        ui_log(UI_LOG_ERROR, "音视频未启动");
    }
}

bool ui_call_busy(void)
{
    return wfav_is_busy();
}

void ui_call_dial(const char *target)
{
    if (wfav_is_busy()) {
        ui_call_open();
        return;
    }
    if (wfav_start_call(target) != ESP_OK) {
        ui_log(UI_LOG_ERROR, "无法发起通话");
    }
}
