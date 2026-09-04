/* 语音消息 -- SoundMessageContent, content type 2.
 *
 * A bubble that says how long the message is and plays when tapped. The
 * bubble gets wider with the message, which every other client does and is
 * worth copying: it is the only thing on screen that tells a three-second
 * message from a two-minute one before you have played either.
 *
 * Everything slow is behind ui_voice.h -- the URL, the download, the decode,
 * the speaker -- on tasks of its own, exactly as a picture's bytes are behind
 * ui_media.h. This file draws.
 *
 * ------------------------------------------------------------------------
 * What is on the wire (WFC.js/wfc/messages/soundMessageContent.js):
 *
 *   remoteMediaUrl   the .amr on the file server. Read from the store rather
 *                    than carried in a row, for the reason ui_msg_image.c
 *                    gives: a fixed field clips the signed URLs some
 *                    deployments issue, and it does it silently.
 *   mediaType        2 (Voice)
 *   content          {"duration":3} -- SECONDS. Android and the web client
 *                    both write seconds, and a client that read them as
 *                    milliseconds would draw every message as 0".
 *
 * The duration is read out of the store in draw(), which ui_msg_view.h allows
 * -- draw may read the store, it may not block -- and which is one indexed
 * lookup on the UID. Carrying it in ui_msg_row_t instead would add a field to
 * the envelope for one type's benefit, which is the thing that header exists
 * to prevent.
 *
 * A sender that leaves the duration out draws without one rather than with a
 * wrong one. There IS a way to recover it -- app_audio_amr_seconds() walks
 * the frame headers -- but it needs the file, and downloading every voice
 * message on the page to find out how long it is defeats the point of a
 * duration field.
 */

#include <stdio.h>
#include <string.h>

#include "ui_msg_view.h"
#include "ui_page.h"
#include "ui_voice.h"
#include "wfc_client.h"

/* A bubble this wide is a message about this long. Under a second still gets
 * the minimum and over half a minute still gets the maximum -- what matters
 * is the comparison between two bubbles on one screen, not the absolute. */
#define BUBBLE_MIN_W 84
#define BUBBLE_MAX_W 200
#define BUBBLE_H     34
#define PX_PER_SEC   4

/* {"duration":3} out of MessageContent.content, by hand.
 *
 * The same call wfc_media.c makes about its one field: this is one integer in
 * an object every client writes the same way, and a JSON parser would be a
 * dependency taken on for it. */
static int duration_of(const char *content)
{
    if (content == NULL) {
        return 0;
    }

    const char *p = strstr(content, "\"duration\"");

    if (p == NULL) {
        return 0;
    }
    p += sizeof("\"duration\"") - 1;
    while (*p == ' ' || *p == ':') {
        p++;
    }

    int seconds = 0;

    while (*p >= '0' && *p <= '9') {
        seconds = seconds * 10 + (*p - '0');
        p++;
    }
    return seconds;
}

static bool take_duration(const wfc_message_t *msg, void *ud)
{
    *(int *)ud = duration_of(msg->content.content);
    return false;
}

/* A UID does not fit in the pointer LVGL carries as event user data.
 *
 * intptr_t is 32 bits on this chip and a WFC message UID is 64, so the cast
 * every other page uses to send an index along with a button --
 * (void *)(uintptr_t)i -- quietly keeps the low half of a UID and throws the
 * rest away. wfc_get_message() then matched nothing and every tap logged
 *
 *   W ui_voice: message -1516240767 has no audio to play
 *
 * with a number that is the surviving 32 bits sign-extended back to 64. So
 * the UID travels in eight bytes of its own, freed with the bubble it belongs
 * to -- LV_EVENT_DELETE reaches every child of a page being torn down, which
 * is how a chat page rebuild gets rid of them. */
static void free_uid(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

/* One tap: play this one, or stop it if it is the one already playing.
 *
 * Tapping a DIFFERENT message while one is playing stops the first and does
 * not start the second, and that is what the audio path can honestly promise:
 * app_audio.c holds its latch until the audio ends, so a start issued now
 * would be refused anyway. Stopping and letting the next tap play beats
 * queueing, which would mean a message beginning several seconds after it was
 * asked for. */
static void tapped(lv_event_t *e)
{
    const int64_t *uid = lv_event_get_user_data(e);

    if (ui_voice_playing()) {
        ui_voice_stop();
        return;
    }
    ui_voice_play(*uid);
}

static void draw(lv_obj_t *parent, const ui_msg_row_t *row)
{
    lv_obj_t *line = ui_msg_line(parent, row);

    /* Same as a picture: in a group there is no writing to recognise the
     * sender by, so the name goes on every one of them. */
    if (!row->mine && row->group) {
        lv_obj_t *who = lv_label_create(line);

        lv_obj_set_style_text_font(who, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(who, lv_color_hex(UI_C_DIM), 0);
        lv_label_set_text(who, row->who);
    }

    int seconds = 0;

    wfc_get_message(row->message_uid, take_duration, &seconds);

    bool playing = row->message_uid != 0 &&
                   ui_voice_playing_uid() == row->message_uid;

    int width = BUBBLE_MIN_W + seconds * PX_PER_SEC;

    if (width > BUBBLE_MAX_W) {
        width = BUBBLE_MAX_W;
    }

    lv_obj_t *bubble = lv_button_create(line);

    lv_obj_set_size(bubble, width, BUBBLE_H);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_set_style_pad_hor(bubble, 10, 0);
    lv_obj_set_style_bg_color(bubble,
                              lv_color_hex(row->mine ? UI_C_ACCENT : UI_C_RAISED), 0);

    int64_t *uid = lv_malloc(sizeof(*uid));

    /* No room for eight bytes means a bubble that draws and does not play,
     * which is the same outcome as a download that fails and is better than
     * one that plays whichever message the low half happens to name. */
    if (uid != NULL) {
        *uid = row->message_uid;
        lv_obj_add_event_cb(bubble, tapped, LV_EVENT_CLICKED, uid);
        lv_obj_add_event_cb(bubble, free_uid, LV_EVENT_DELETE, uid);
    }

    lv_obj_t *label = lv_label_create(bubble);
    char      text[32];

    /* The symbol carries the state so the words do not have to: a bubble that
     * said 「播放中」 would be a bubble that changes width when it starts. */
    if (seconds > 0) {
        snprintf(text, sizeof(text), "%s %d\"",
                 playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY, seconds);
    } else {
        snprintf(text, sizeof(text), "%s 语音",
                 playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(row->mine ? 0xFFFFFF : UI_C_TEXT), 0);
    lv_obj_center(label);

    ui_msg_receipt(line, row);
}

const ui_msg_view_t ui_msg_view_voice = {
    .type = WFC_CONTENT_VOICE,
    .draw = draw,
};

/* 对讲语音 -- content type 23, which is a SoundMessageContent with a
 * different number on it (ptt.js's pttSoundMessageContent.js). One press of
 * the 对讲 button leaves one behind, so a channel keeps a record for whoever
 * was not listening at the time.
 *
 * Same view, and that is the point: remoteMediaUrl, mediaType 2 and
 * {"duration":N} are all in the same places, so there is nothing for a second
 * drawing routine to do. It is a separate entry rather than a second `type`
 * field on the one above because the table is a flat list of (type, draw)
 * pairs and keeping it that way costs one struct in .rodata.
 *
 * The number is spelled out here rather than taken from wfptt_types.h because
 * this file is in every build: a board with CONFIG_APP_PTT=n cannot talk, but
 * it can still be talked AT, and the message it keeps should still play. */
const ui_msg_view_t ui_msg_view_ptt_sound = {
    .type = 23,
    .draw = draw,
};
