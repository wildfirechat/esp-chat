/* 对讲 -- one channel, one big button.
 *
 * The page is the shape push-to-talk has had since the first handset: hold to
 * talk, let go to listen, and a line that says which of the two is happening.
 * Everything under it is ../../wfptt-esp; this file draws and posts.
 *
 * ------------------------------------------------------------------------
 * Press and release, not tap.
 *
 * The record page (ui_record.c) is a tap to start and a tap to stop, because
 * a voice message is composed and then sent. This is not that: the button
 * IS the microphone, so it is LV_EVENT_PRESSED and LV_EVENT_RELEASED -- and
 * LV_EVENT_PRESS_LOST beside the second one, which is the finger sliding off
 * the button. A press-lost that was not treated as a release is a board that
 * keeps talking until the cap, and it is the single easiest thing to get
 * wrong here.
 *
 * Both are safe from the LVGL task, which is why they are the only two things
 * the button does: wfptt_request_talk() and wfptt_release_talk() post to the
 * SDK's task and return. Everything they set in motion -- a lock round trip,
 * a microphone, a message every 400 ms -- happens somewhere else, and this
 * page finds out by repainting.
 *
 * The one thing that is NOT safe from a button is muting the channel: it
 * writes a user setting, which reaches a blocking send on the long link. So
 * it is parked and done in prime(), the same way the chat page sends.
 *
 * ------------------------------------------------------------------------
 * Leaving the page does not leave the channel.
 *
 * Listening is global (wfptt_client.h): a board goes on hearing its channels
 * with the conversation list on screen, which is what makes it a walkie-talkie
 * rather than a screen you have to be looking at. What destroy() does have to
 * do is let go of the button -- a page torn down mid-press would otherwise
 * leave the microphone open.
 */

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "ui_page.h"
#include "wfptt.h"

/* As many names as fit on one line under the button. A channel with more
 * talking than that says so as a number instead. */
#define SHOW_TALKERS 3

static wfc_conversation_t s_conv;

static lv_obj_t *s_state;
static lv_obj_t *s_button;
static lv_obj_t *s_button_label;
static lv_obj_t *s_hint;
static lv_obj_t *s_mute;
static lv_obj_t *s_mute_label;

/* Set by the mute button, acted on by prime(). */
static bool s_toggle_mute;
/* What the mute button drew last, so prime() knows what to write. */
static bool s_silent;

/* ------------------------------------------------------------ the button */

static void pressed(lv_event_t *e)
{
    (void)e;
    wfptt_request_talk(&s_conv);
    ui_dirty(UI_DIRTY_PTT);
}

/* Both LV_EVENT_RELEASED and LV_EVENT_PRESS_LOST land here. The second one is
 * the finger leaving the button without leaving the screen, and it is a
 * release in every way that matters to a microphone. */
static void released(lv_event_t *e)
{
    (void)e;
    wfptt_release_talk();
    ui_dirty(UI_DIRTY_PTT);
}

static void mute_clicked(lv_event_t *e)
{
    (void)e;
    s_toggle_mute = true;
    ui_dirty(UI_DIRTY_PTT);
}

/* -------------------------------------------------------------- the page */

void ui_ptt_open(const wfc_conversation_t *conv)
{
    s_conv        = *conv;
    s_toggle_mute = false;
    ui_goto(UI_PAGE_PTT);
}

static void create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 6, 0);

    s_state = lv_label_create(parent);
    lv_obj_set_style_text_color(s_state, lv_color_hex(UI_C_TEXT), 0);

    /* Bigger than the record page's, and deliberately: this one is held down
     * rather than tapped, and a target that has to stay under a thumb wants
     * the room. */
    s_button = lv_button_create(parent);
    lv_obj_set_size(s_button, 108, 108);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_button, 0, 0);
    lv_obj_add_event_cb(s_button, pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_button, released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_button, released, LV_EVENT_PRESS_LOST, NULL);

    s_button_label = lv_label_create(s_button);
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_button_label);

    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_C_DIM), 0);
    lv_obj_set_style_text_font(s_hint, UI_FONT_SMALL, 0);

    s_mute = lv_button_create(parent);
    lv_obj_set_height(s_mute, 28);
    lv_obj_set_style_bg_color(s_mute, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_radius(s_mute, 6, 0);
    lv_obj_set_style_shadow_width(s_mute, 0, 0);
    lv_obj_set_style_pad_hor(s_mute, 12, 0);
    lv_obj_add_event_cb(s_mute, mute_clicked, LV_EVENT_CLICKED, NULL);

    s_mute_label = lv_label_create(s_mute);
    lv_obj_set_style_text_color(s_mute_label, lv_color_hex(UI_C_TEXT), 0);
    lv_obj_center(s_mute_label);

    s_silent = wfptt_is_silent(&s_conv);
}

/* Who is talking, as names rather than IDs. Reads the store, which draw() is
 * allowed to do -- wfc_get_display_name() never fetches (wfc_client.h), which
 * is exactly why it may be called from here. The profiles it needs are asked
 * for in prime(). */
static void describe_talkers(char *buf, size_t buf_size)
{
    char   uids[SHOW_TALKERS][WFC_TARGET_MAX];
    size_t n = wfptt_talkers(&s_conv, uids, SHOW_TALKERS);

    if (n == 0) {
        strlcpy(buf, "按住说话", buf_size);
        return;
    }

    buf[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        char who[WFC_NAME_MAX];

        wfc_get_display_name(uids[i],
                             s_conv.type == WFC_CONV_GROUP ? s_conv.target : NULL,
                             who, sizeof(who));
        if (i > 0) {
            strlcat(buf, "、", buf_size);
        }
        strlcat(buf, who, buf_size);
    }
    strlcat(buf, " 正在讲话", buf_size);
}

static void refresh(uint32_t dirty)
{
    /* STATUS is the one-second tick, which is what moves the elapsed count;
     * PTT is the channel itself; NAMES is a profile arriving and turning an
     * ID into a name. */
    if ((dirty & (UI_DIRTY_PTT | UI_DIRTY_STATUS | UI_DIRTY_NAMES)) == 0 ||
        s_state == NULL) {
        return;
    }

    wfptt_status_t st;

    wfptt_status(&st);

    bool connected = wfc_client_status() == WFC_STATUS_CONNECTED ||
                     wfc_client_status() == WFC_STATUS_RECEIVING;

    char line[96];

    switch (st.state) {
    case WFPTT_TALKING:
        /* The countdown only appears near the end, the same rule the record
         * page follows: a number counting down from sixty is noise, one that
         * appears at ten is a warning. */
        if (st.max_seconds - st.talk_seconds <= 10) {
            snprintf(line, sizeof(line), "讲话中 %d\"  还剩 %d 秒", st.talk_seconds,
                     st.max_seconds - st.talk_seconds);
        } else {
            snprintf(line, sizeof(line), "讲话中 %d\"", st.talk_seconds);
        }
        lv_label_set_text(s_button_label, "松开");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(UI_C_BAD), 0);
        break;
    case WFPTT_REQUESTING:
        strlcpy(line, "正在占用频道…", sizeof(line));
        lv_label_set_text(s_button_label, "…");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(UI_C_RAISED), 0);
        break;
    case WFPTT_IDLE:
    default:
        if (!connected) {
            strlcpy(line, "未连接", sizeof(line));
        } else if (st.speaker[0] != '\0') {
            char who[WFC_NAME_MAX];

            wfc_get_display_name(st.speaker,
                                 st.speaker_conv.type == WFC_CONV_GROUP
                                     ? st.speaker_conv.target
                                     : NULL,
                                 who, sizeof(who));
            snprintf(line, sizeof(line), "%s 在讲话", who);
        } else {
            strlcpy(line, "空闲", sizeof(line));
        }
        lv_label_set_text(s_button_label, "讲话");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(UI_C_ACCENT), 0);
        break;
    }
    lv_label_set_text(s_state, line);

    if (connected) {
        lv_obj_remove_state(s_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_button, LV_STATE_DISABLED);
    }

    char hint[96];

    describe_talkers(hint, sizeof(hint));
    lv_label_set_text(s_hint, hint);

    s_silent = wfptt_is_silent(&s_conv);
    lv_label_set_text(s_mute_label, s_silent ? "取消静音" : "静音");
    lv_obj_set_style_text_color(
        s_mute_label, lv_color_hex(s_silent ? UI_C_BAD : UI_C_TEXT), 0);
}

static void prime(void)
{
    /* The names on this page belong to whoever is talking, and they are the
     * one thing here that may need the server. Asked from prime() like every
     * other page's, so a cache miss becomes a UPUI and a repaint rather than
     * a blocking call under the display lock. */
    char   uids[SHOW_TALKERS][WFC_TARGET_MAX];
    size_t n = wfptt_talkers(&s_conv, uids, SHOW_TALKERS);

    for (size_t i = 0; i < n; i++) {
        wfc_user_info_t user;

        wfc_get_user_info(uids[i], false, &user);
    }

    if (s_toggle_mute) {
        s_toggle_mute = false;
        /* Sent, not set: the row appears when the server acknowledges it, and
         * a refused change simply does not happen. The label follows the
         * store on the next repaint rather than being flipped here. */
        wfptt_set_silent(&s_conv, !s_silent);
    }
}

static void destroy(void)
{
    /* Leaving the page mid-press. The channel goes on being listened to --
     * that is deliberate -- but the microphone must not be left open by a
     * button that is about to be deleted. */
    wfptt_release_talk();

    s_state        = NULL;
    s_button       = NULL;
    s_button_label = NULL;
    s_hint         = NULL;
    s_mute         = NULL;
    s_mute_label   = NULL;
    s_toggle_mute  = false;
}

static void title(char *buf, size_t buf_size)
{
    char name[UI_TITLE_MAX];

    wfc_get_conversation_title(&s_conv, name, sizeof(name));
    lv_snprintf(buf, (uint32_t)buf_size, "对讲 %s", name);
}

const ui_page_def_t ui_page_ptt = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
