/* One conversation: the bubbles, and the way to add to them.
 *
 * Same shape as the list -- the store is the model, a redraw is a re-read of
 * the last MSG_MAX messages, and nothing is patched in place. A message
 * arriving, a message being sent, and a name being resolved all end up in the
 * same place: rebuild from the store.
 *
 * Sending is deliberately not done here. This page collects nothing; it opens
 * the composer (ui_compose.c) and is handed back a string. That seam is where
 * a voice message goes later: recording, encoding and uploading is a
 * different page with the same one-line contract, and this file does not
 * change.
 *
 * Drawing is not all done here either. This file knows two shapes -- a bubble
 * and a centred notice -- and everything else is a view, looked up by content
 * type in ui_msg_view.h. The row this page keeps is the envelope and nothing
 * else: a view that needs more than the envelope reads it back out of the
 * store, by message_uid, from prime().
 *
 * Opening the conversation marks it read, which is what every client does and
 * what makes the unread badge mean something. That now also tells the server
 * -- wfc_clear_unread() reports it, so the phone stops badging the same
 * conversation and the sender gets a read receipt -- but it is still one call
 * and it still does not block, so nothing here changes.
 *
 * Coming back the other way: a message we sent carries how far the other side
 * has got, read out of the store while the rows are built and drawn by
 * ui_msg_receipt() at the end of every view.
 *
 * The buttons beside the input are a microphone (always), 对讲 (in a build
 * with the PTT SDK, on both kinds of conversation) and then whichever of two
 * this conversation has: a phone for a single chat, and 群 for a group -- the
 * way to the roster and to the three things that change it. The last two
 * never both apply.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_media.h"
#include "ui_page.h"
#include "ui_msg_view.h"
#include "ui_voice.h"
#include "wfc_mem.h"

#include "custom_message.h"

/* How far back the page reads. A 320x240 panel shows four or five bubbles, so
 * this is "enough to scroll through", not "the whole history" -- and it is
 * re-read on every repaint, so it wants to stay cheap. */
#define MSG_MAX  30

/* Room for the longest thing the composer will hand over, so parking it
 * never has to cut a character in half. */
#define PENDING_MAX 256

static wfc_conversation_t s_conv;
static lv_obj_t          *s_msgs;
static lv_obj_t          *s_input;
static lv_obj_t          *s_input_label;
/* Only on a single conversation: there is no group call in this build, and a
 * button that always declines is worse than no button. */
static lv_obj_t          *s_call;
static lv_obj_t          *s_call_label;
/* On both kinds of conversation, and only in a build with the PTT SDK. */
static lv_obj_t          *s_ptt;
static lv_obj_t          *s_ptt_label;
static ui_msg_row_t      *s_rows;      /* MSG_MAX of them, in PSRAM */
static size_t             s_count;
/* Written by the composer's callback, drained by prime(). */
static char               s_pending[PENDING_MAX];
/* Asked once per repaint rather than once per row: it reads a user setting
 * out of the store, and the answer is the same for all thirty of them. */
static bool               s_receipts;

/* --------------------------------------------------------------- reading */

/* The store answers newest first and the page reads top to bottom, so this
 * fills backwards and refresh() draws the tail it filled. */
static bool collect(const wfc_message_t *msg, void *ud)
{
    (void)ud;

    ui_msg_row_t *row = &s_rows[MSG_MAX - 1 - s_count];

    row->mine        = msg->direction == WFC_DIRECTION_SEND;
    row->timestamp   = msg->timestamp;
    row->message_uid = msg->message_uid;
    row->type        = msg->content.type;
    row->group       = s_conv.type == WFC_CONV_GROUP;
    /* Asked of the type table rather than tested against type numbers here:
     * a custom type registered as a notification is centred like the built-in
     * ones, and this page does not have to learn about it. */
    row->notice      = wfc_content_is_notification(msg->content.type);
    /* Receipts, for our own messages only. Two store lookups per row, and
     * they are here rather than in draw() because this is the pass that is
     * allowed to read the store deeply -- and because a row that already
     * knows costs a view nothing. On a deployment without receipts they would
     * each answer "nothing yet" anyway; s_receipts is what stops them being
     * asked thirty times to be told so. */
    if (row->mine && s_receipts) {
        row->receipt = (int8_t)wfc_message_receipt(&s_conv, msg->timestamp);
        row->read_by = (uint8_t)wfc_message_read_count(&s_conv, msg->timestamp);
    }

    strlcpy(row->from, row->mine ? "" : msg->from, sizeof(row->from));
    wfc_get_display_name(row->mine ? wfc_client_user_id() : msg->from,
                         row->group ? s_conv.target : NULL,
                         row->who, sizeof(row->who));
    /* The one line of text every row has, and the same one the conversation
     * list shows. A type that wants different words changes its digest
     * (wfc_content.h); a type that wants to look different gets a view
     * (ui_msg_view.h). Neither is this page's business. */
    wfc_message_digest(msg, row->text, sizeof(row->text));

    return ++s_count < MSG_MAX;
}

/* The page's prime(): off both locks, so it may block. Two jobs -- ask for
 * the senders whose names did not resolve, and put out the message the
 * composer handed over. Sending is here rather than in the composer's button
 * callback for the same reason the fetching is: wfc_send_text() ends in a
 * blocking send(), and the button callback runs on the LVGL task holding the
 * display lock. */
static void prime(void)
{
    if (s_rows != NULL) {
        for (size_t i = MSG_MAX - s_count; i < MSG_MAX; i++) {
            if (s_rows[i].from[0] != '\0') {
                wfc_user_info_t user;

                wfc_get_user_info(s_rows[i].from, false, &user);
            }
        }
    }

    if (s_pending[0] != '\0') {
        /* Two typed words send the two example custom messages instead of a
         * line of text, which is how they are tried on a board without a
         * button that only a demonstration would need. See
         * wfc_custom_message/README.md. */
        esp_err_t err;

        if (strcmp(s_pending, "/custom") == 0) {
            err = custom_message_send_test(&s_conv, "这是一条自定义消息");
        } else if (strcmp(s_pending, "/tip") == 0) {
            err = custom_message_send_test_notification(&s_conv, "这是一条自定义通知");
        } else {
            err = wfc_send_text(&s_conv, s_pending);
        }

        s_pending[0] = '\0';
        if (err != ESP_OK) {
            ui_logf(UI_LOG_ERROR, "发送未能提交: %s", esp_err_to_name(err));
            return;
        }
        /* Answering a conversation is reading it, and the list should say so
         * without waiting for the server. The bubble itself appears when the
         * server names the message -- until then it has no UID to store it
         * under. */
        wfc_clear_unread(&s_conv);
    }
}

/* --------------------------------------------------------------- drawing */

static void draw_notice(const ui_msg_row_t *row)
{
    lv_obj_t *label = lv_label_create(s_msgs);

    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_C_DIM), 0);
    lv_label_set_text(label, row->text);
}

/* `show_who` is false when the previous bubble was from the same person: a
 * run of messages from one sender reads better without their name over every
 * one of them. */
static void draw_bubble(const ui_msg_row_t *row, bool show_who)
{
    lv_obj_t *line = ui_msg_line(s_msgs, row);

    if (show_who && !row->mine && row->group) {
        lv_obj_t *who = lv_label_create(line);

        lv_obj_set_style_text_font(who, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(who, lv_color_hex(UI_C_DIM), 0);
        lv_label_set_text(who, row->who);
    }

    lv_obj_t *bubble = lv_label_create(line);

    lv_label_set_long_mode(bubble, LV_LABEL_LONG_MODE_WRAP);
    /* Not the full width: a bubble that reaches both edges stops reading as
     * a bubble, and the gap on the far side is what says who sent it. */
    lv_obj_set_style_max_width(bubble, LV_PCT(78), 0);
    lv_obj_set_style_bg_color(
        bubble, lv_color_hex(row->mine ? UI_C_ACCENT : UI_C_RAISED), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 7, 0);
    lv_obj_set_style_text_color(
        bubble, lv_color_hex(row->mine ? 0xFFFFFF : UI_C_TEXT), 0);
    lv_label_set_text(bubble, row->text);

    ui_msg_receipt(line, row);
}

/* ---------------------------------------------------------------- input */

/* The composer hands the text back here, on the LVGL task with the display
 * lock held -- so this only parks it. prime() puts it on the wire a tick
 * later, off the lock. */
static void send_text(const char *text)
{
    if (text != NULL) {
        wfc_copy_text(s_pending, sizeof(s_pending), text);
    }
}

static void input_clicked(lv_event_t *e)
{
    (void)e;

    char title[UI_TITLE_MAX];

    wfc_get_conversation_title(&s_conv, title, sizeof(title));
    ui_compose_open(title, send_text);
}

/* The recorder hands the audio back here. Unlike send_text() this does not
 * park anything for prime(): a voice message is an upload, which is seconds,
 * and prime() runs on the UI task. ui_voice_send() takes the buffer and
 * returns, and the conversation goes with it -- so leaving this page while it
 * is still going up is fine.
 *
 * The conversation is read here rather than inside ui_voice.c for the reason
 * §8.13 wrote down about the composer: this runs while the RECORD page is
 * current, so s_conv is still whatever this page was opened for. */
static void send_voice(uint8_t *amr, size_t len, int seconds)
{
    ui_voice_send(&s_conv, amr, len, seconds);
}

static void mic_clicked(lv_event_t *e)
{
    (void)e;

    char title[UI_TITLE_MAX];

    wfc_get_conversation_title(&s_conv, title, sizeof(title));
    ui_record_open(title, send_voice);
}

/* 对讲. Unlike the phone beside it this starts nothing -- the page is where
 * the button that talks is -- so it is a plain navigation, and it is offered
 * on a group as readily as on a single chat: a channel with three people on
 * it is what push-to-talk is for. */
static void ptt_clicked(lv_event_t *e)
{
    (void)e;
    ui_ptt_open(&s_conv);
}

/* Placing a call, or getting back to one that is already up -- ui_call_dial()
 * is both, and it is a post either way, which it has to be: this runs on the
 * LVGL task with the display lock held, and starting a call sends an IM
 * message and then talks to a TURN server. */
static void call_clicked(lv_event_t *e)
{
    (void)e;
    ui_call_dial(s_conv.target);
}

/* The same slot as the phone button, which a group never has: there is no
 * group call in this build, and a group is the conversation with somewhere
 * else to go -- the roster and the three things that change it (ui_group.c). */
static void group_clicked(lv_event_t *e)
{
    (void)e;
    ui_group_open(s_conv.target);
}

/* ------------------------------------------------------------ the page */

void ui_chat_open(const wfc_conversation_t *conv)
{
    s_conv = *conv;
    /* Anything the composer parked belonged to the conversation being left,
     * so it does not follow us into this one. */
    s_pending[0] = '\0';
    ui_goto(UI_PAGE_CHAT);
}

static void create(lv_obj_t *parent)
{
    s_rows  = wfc_calloc(MSG_MAX, sizeof(*s_rows));
    s_count = 0;

    s_msgs = lv_obj_create(parent);
    ui_style_flat(s_msgs);
    lv_obj_set_width(s_msgs, LV_PCT(100));
    lv_obj_set_flex_grow(s_msgs, 1);
    lv_obj_set_style_pad_all(s_msgs, 6, 0);
    lv_obj_set_style_pad_row(s_msgs, 6, 0);
    lv_obj_set_flex_flow(s_msgs, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_msgs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_msgs, LV_DIR_VER);

    lv_obj_t *bar = lv_obj_create(parent);
    ui_style_flat(bar);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_C_LINE), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 5, 0);
    lv_obj_set_style_pad_column(bar, 5, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* One wide target rather than a field plus a send button: there is no
     * room for a keyboard on this page anyway, so tapping anywhere along it
     * opens the composer, which is where the keyboard lives. The call button
     * beside it is the exception -- it is the one action that must not be
     * reachable by a stray tap on the text field. */
    s_input = lv_button_create(bar);
    lv_obj_set_height(s_input, LV_PCT(100));
    lv_obj_set_flex_grow(s_input, 1);
    lv_obj_set_style_bg_color(s_input, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_shadow_width(s_input, 0, 0);
    lv_obj_set_style_radius(s_input, 6, 0);
    lv_obj_add_event_cb(s_input, input_clicked, LV_EVENT_CLICKED, NULL);

    s_input_label = lv_label_create(s_input);
    lv_obj_set_style_text_color(s_input_label, lv_color_hex(UI_C_DIM), 0);
    lv_label_set_text(s_input_label, "输入消息…");
    lv_obj_align(s_input_label, LV_ALIGN_LEFT_MID, 4, 0);

    /* Always there, on both kinds of conversation and in every build: a
     * voice message needs the microphone but not the AV SDK, so unlike the
     * phone beside it this button does not come and go. */
    lv_obj_t *mic = lv_button_create(bar);

    lv_obj_set_size(mic, 40, LV_PCT(100));
    lv_obj_set_style_bg_color(mic, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_shadow_width(mic, 0, 0);
    lv_obj_set_style_radius(mic, 6, 0);
    lv_obj_add_event_cb(mic, mic_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mic_label = lv_label_create(mic);

    lv_obj_set_style_text_color(mic_label, lv_color_hex(UI_C_DIM), 0);
    lv_label_set_text(mic_label, LV_SYMBOL_AUDIO);
    lv_obj_center(mic_label);

    /* Both kinds of conversation, and only in a build that has the PTT SDK:
     * with CONFIG_APP_PTT=n there is nothing behind it, so there is no button
     * (ui_page.h). It is a separate switch from the phone's -- a talk needs
     * the microphone, not WebRTC. */
    if (ui_ptt_available()) {
        s_ptt = lv_button_create(bar);
        lv_obj_set_size(s_ptt, 46, LV_PCT(100));
        lv_obj_set_style_bg_color(s_ptt, lv_color_hex(UI_C_RAISED), 0);
        lv_obj_set_style_shadow_width(s_ptt, 0, 0);
        lv_obj_set_style_radius(s_ptt, 6, 0);
        lv_obj_set_style_pad_hor(s_ptt, 2, 0);
        lv_obj_add_event_cb(s_ptt, ptt_clicked, LV_EVENT_CLICKED, NULL);

        s_ptt_label = lv_label_create(s_ptt);
        lv_obj_set_style_text_color(s_ptt_label, lv_color_hex(UI_C_DIM), 0);
        lv_label_set_text(s_ptt_label, "对讲");
        lv_obj_center(s_ptt_label);
    }

    /* One-to-one only, and only in a build that has the AV SDK: with
     * CONFIG_APP_CALL=n there is nothing behind the button, so there is no
     * button (ui_page.h). Everything below that touches it is already written
     * to cope with it not being there. */
    if (s_conv.type == WFC_CONV_SINGLE && ui_call_available()) {
        s_call = lv_button_create(bar);
        lv_obj_set_size(s_call, 46, LV_PCT(100));
        lv_obj_set_style_bg_color(s_call, lv_color_hex(UI_C_RAISED), 0);
        lv_obj_set_style_shadow_width(s_call, 0, 0);
        lv_obj_set_style_radius(s_call, 6, 0);
        lv_obj_add_event_cb(s_call, call_clicked, LV_EVENT_CLICKED, NULL);

        s_call_label = lv_label_create(s_call);
        lv_obj_set_style_text_color(s_call_label, lv_color_hex(UI_C_OK), 0);
        lv_label_set_text(s_call_label, LV_SYMBOL_CALL);
        lv_obj_center(s_call_label);
    } else if (s_conv.type == WFC_CONV_GROUP) {
        lv_obj_t *group = lv_button_create(bar);

        lv_obj_set_size(group, 46, LV_PCT(100));
        lv_obj_set_style_bg_color(group, lv_color_hex(UI_C_RAISED), 0);
        lv_obj_set_style_shadow_width(group, 0, 0);
        lv_obj_set_style_radius(group, 6, 0);
        lv_obj_add_event_cb(group, group_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *label = lv_label_create(group);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_C_DIM), 0);
        lv_label_set_text(label, "群");
        lv_obj_center(label);
    }

    /* Opening a conversation reads it. */
    wfc_clear_unread(&s_conv);
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_MESSAGES | UI_DIRTY_NAMES | UI_DIRTY_CONVS |
                  UI_DIRTY_MEDIA | UI_DIRTY_PTT)) == 0) {
        return;
    }
    if (s_rows == NULL) {
        return;
    }

    bool connected = wfc_client_status() == WFC_STATUS_CONNECTED ||
                     wfc_client_status() == WFC_STATUS_RECEIVING;

    lv_label_set_text(s_input_label, connected ? "输入消息…" : "未连接，不能发送");
    if (connected) {
        lv_obj_remove_state(s_input, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_input, LV_STATE_DISABLED);
    }

    if (s_ptt != NULL) {
        /* Lit while this board is talking or hearing somebody -- on any
         * channel, since listening is global. It is the only thing on this
         * page that says a talk is happening while the page itself is not
         * the 对讲 one. */
        bool busy = ui_ptt_busy();

        lv_obj_set_style_text_color(
            s_ptt_label, lv_color_hex(busy ? UI_C_IN : UI_C_DIM), 0);
        if (connected) {
            lv_obj_remove_state(s_ptt, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_ptt, LV_STATE_DISABLED);
        }
    }

    if (s_call != NULL) {
        /* A call in progress turns the button into the way back to it, which
         * is what makes the back arrow on the call page a minimise rather
         * than a hang-up. */
        bool busy = ui_call_busy();

        lv_obj_set_style_text_color(
            s_call_label, lv_color_hex(busy ? UI_C_ACCENT : UI_C_OK), 0);
        if (connected || busy) {
            lv_obj_remove_state(s_call, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_call, LV_STATE_DISABLED);
        }
    }

    s_count    = 0;
    s_receipts = wfc_is_receipt_enabled();
    memset(s_rows, 0, MSG_MAX * sizeof(*s_rows));
    wfc_get_messages(&s_conv, MSG_MAX, collect, NULL);

    lv_obj_clean(s_msgs);

    /* Every widget that could have been holding a picture has just been
     * deleted and none of the new ones exist yet, which is the one moment a
     * picture may be freed (ui_media.h). What scrolled out of the window goes
     * back to the heap here. */
    int64_t on_screen[MSG_MAX];

    for (size_t i = 0; i < s_count; i++) {
        on_screen[i] = s_rows[MSG_MAX - s_count + i].message_uid;
    }
    ui_media_keep_only(on_screen, s_count);

    if (s_count == 0) {
        lv_obj_t *empty = ui_label(s_msgs, "还没有消息", UI_C_DIM);

        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(empty, 12, 0);
    }

    /* Three shapes, most specific first: a type with a view of its own draws
     * itself, a type registered as a notification is a centred line, and
     * everything else is a bubble. The run-of-messages rule is reset by the
     * first two, since neither carries the sender's name the way a bubble
     * does. */
    const char *previous = NULL;
    for (size_t i = MSG_MAX - s_count; i < MSG_MAX; i++) {
        const ui_msg_row_t *row = &s_rows[i];

        if (ui_msg_view_draw(s_msgs, row)) {
            previous = NULL;
            continue;
        }
        if (row->notice) {
            draw_notice(row);
            previous = NULL;
            continue;
        }
        draw_bubble(row, previous == NULL || strcmp(previous, row->who) != 0);
        previous = row->who;
    }

    /* A name arriving should not yank the view down while someone is reading
     * back through the history; a message arriving should. */
    if ((dirty & UI_DIRTY_MESSAGES) != 0 && lv_obj_get_child_count(s_msgs) > 0) {
        lv_obj_update_layout(s_msgs);
        lv_obj_scroll_to_view(lv_obj_get_child(s_msgs, -1), LV_ANIM_OFF);
    }
}

static void destroy(void)
{
    /* Leaving the conversation gives every picture back. The widgets holding
     * them are deleted with the page, before this runs. */
    ui_media_keep_only(NULL, 0);

    wfc_free(s_rows);
    s_rows        = NULL;
    s_msgs        = NULL;
    s_input       = NULL;
    s_input_label = NULL;
    s_call        = NULL;
    s_call_label  = NULL;
    s_ptt         = NULL;
    s_ptt_label   = NULL;
    s_count       = 0;
}

static void title(char *buf, size_t buf_size)
{
    wfc_get_conversation_title(&s_conv, buf, buf_size);
}

const ui_page_def_t ui_page_chat = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
