/* Recording one voice message.
 *
 * The chat page's header says a voice message would be "a different page with
 * the same one-line contract" as the composer, and this is it: ui_record_open()
 * takes a callback, the page collects something, the callback gets it, the
 * page goes away. ui_chat.c gained a button and nothing else.
 *
 * What is different from the composer is what "collect" costs. Text is free
 * and arrives complete; audio arrives at 50 frames a second from a task that
 * has the microphone open, so this page has a running recording behind it and
 * three ways out of one -- send it, throw it away, or hit the cap -- and all
 * three have to put the microphone down. That is app_audio.c's job; this file
 * only ever says start, stop or cancel, and draws what it is told.
 *
 * The elapsed count rides the shell's one-second tick (UI_DIRTY_STATUS) rather
 * than a timer of its own. A recording is measured in whole seconds, the tick
 * is already there for the clock, and a page that needs a second mechanism to
 * count to sixty is a page that has gone wrong.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "app_audio.h"
#include "ui.h"
#include "ui_page.h"
#include "wfc_mem.h"

static const char *TAG = "ui_record";

static char            s_title[UI_TITLE_MAX];
static ui_record_cb_t  s_on_done;

static lv_obj_t *s_state;    /* 「按住说话」 / 「录音中」 */
static lv_obj_t *s_elapsed;
static lv_obj_t *s_button;
static lv_obj_t *s_button_label;
static lv_obj_t *s_hint;

/* Set by the button, acted on by prime(). The recording has to be stopped
 * somewhere that may block -- record_stop() waits for the recorder task to
 * close the microphone -- and a button callback runs on the LVGL task, which
 * is the one place on this board where blocking looks like a crash. */
static enum { IDLE, ARMED_STOP, ARMED_CANCEL } s_pending;

/* Whether there is a recording to collect. Distinguishes "nothing has been
 * started" from "the cap stopped it", which look the same from
 * app_audio_recording(). */
static bool s_started;

/* ------------------------------------------------------------ the button */

static void button_clicked(lv_event_t *e)
{
    (void)e;

    if (app_audio_recording()) {
        s_pending = ARMED_STOP;
    } else if (s_pending == IDLE) {
        if (app_audio_record_start() == ESP_OK) {
            s_started = true;
        } else {
            ui_log(UI_LOG_ERROR, "录音无法开始：设备忙");
            ui_back();
        }
    }
    ui_dirty(UI_DIRTY_STATUS);
}

static void cancel_clicked(lv_event_t *e)
{
    (void)e;
    s_pending = ARMED_CANCEL;
    ui_dirty(UI_DIRTY_STATUS);
}

/* -------------------------------------------------------------- the page */

void ui_record_open(const char *title, ui_record_cb_t on_done)
{
    strlcpy(s_title, title != NULL ? title : "", sizeof(s_title));
    s_on_done = on_done;
    s_pending = IDLE;
    s_started = false;
    ui_goto(UI_PAGE_RECORD);
}

static void create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 8, 0);

    s_state = lv_label_create(parent);
    lv_obj_set_style_text_color(s_state, lv_color_hex(UI_C_TEXT), 0);

    s_elapsed = lv_label_create(parent);
    lv_obj_set_style_text_color(s_elapsed, lv_color_hex(UI_C_DIM), 0);

    s_button = lv_button_create(parent);
    lv_obj_set_size(s_button, 96, 96);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_button, 0, 0);
    lv_obj_add_event_cb(s_button, button_clicked, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_button_label);

    s_hint = lv_button_create(parent);
    lv_obj_set_height(s_hint, 28);
    lv_obj_set_style_bg_color(s_hint, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_radius(s_hint, 6, 0);
    lv_obj_set_style_shadow_width(s_hint, 0, 0);
    lv_obj_set_style_pad_hor(s_hint, 12, 0);
    lv_obj_add_event_cb(s_hint, cancel_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint_label = lv_label_create(s_hint);

    lv_label_set_text(hint_label, "取消");
    lv_obj_set_style_text_color(hint_label, lv_color_hex(UI_C_TEXT), 0);
    lv_obj_center(hint_label);
}

static void refresh(uint32_t dirty)
{
    /* STATUS is the one-second tick; the page has nothing in the store. */
    if ((dirty & UI_DIRTY_STATUS) == 0 || s_state == NULL) {
        return;
    }

    bool recording = app_audio_recording();
    int  seconds   = app_audio_record_seconds();

    lv_label_set_text(s_state, recording ? "录音中" : "点击开始录音");
    lv_label_set_text(s_button_label, recording ? "停止" : "录音");
    lv_obj_set_style_bg_color(s_button,
                              lv_color_hex(recording ? UI_C_BAD : UI_C_ACCENT), 0);

    if (recording) {
        char buf[64];

        /* The countdown only appears near the end. A number that says "you
         * have 47 seconds left" the whole way through is noise; one that
         * appears at ten is a warning. */
        if (APP_AUDIO_MAX_SECONDS - seconds <= 10) {
            snprintf(buf, sizeof(buf), "%d\"  还剩 %d 秒", seconds,
                     APP_AUDIO_MAX_SECONDS - seconds);
        } else {
            snprintf(buf, sizeof(buf), "%d\"", seconds);
        }
        lv_label_set_text(s_elapsed, buf);
    } else {
        lv_label_set_text(s_elapsed, "最长 60 秒");
    }
}

/* Hands `amr` to whoever opened the page and leaves. Ownership goes with it;
 * the chat page frees it once it has been sent. */
static void deliver(uint8_t *amr, size_t len, int seconds)
{
    if (s_on_done != NULL) {
        s_on_done(amr, len, seconds);
    } else {
        wfc_free(amr);
    }
    ui_back();
}

static void prime(void)
{
    if (s_pending == ARMED_CANCEL) {
        s_pending = IDLE;
        app_audio_record_cancel();
        ui_back();
        return;
    }

    /* Two ways to arrive at "stop and send": the button, and the cap. The
     * recorder task stops itself at the cap, so the second one announces
     * itself only as a recording that was started and is no longer running --
     * there is nothing to press and nobody to notice. Both end up here. */
    if (s_pending != ARMED_STOP && !(s_started && !app_audio_recording())) {
        return;
    }

    bool by_cap = s_pending != ARMED_STOP;

    s_pending = IDLE;
    s_started = false;

    uint8_t  *amr     = NULL;
    size_t    len     = 0;
    int       seconds = 0;
    esp_err_t err     = app_audio_record_stop(&amr, &len, &seconds);

    if (err == ESP_ERR_INVALID_SIZE) {
        ui_log(UI_LOG_NOTE, "录音太短，已丢弃");
        ui_back();
        return;
    }
    if (err != ESP_OK) {
        ui_log(UI_LOG_ERROR, "录音失败");
        ui_back();
        return;
    }
    if (by_cap) {
        ESP_LOGI(TAG, "the cap stopped the recording at %d s", seconds);
    }
    deliver(amr, len, seconds);
}

static void destroy(void)
{
    /* Leaving by the back arrow rather than by a button: the recording is
     * still running and nobody is going to ask for it. */
    app_audio_record_cancel();

    s_state        = NULL;
    s_elapsed      = NULL;
    s_button       = NULL;
    s_button_label = NULL;
    s_hint         = NULL;
    s_pending      = IDLE;
    s_started      = false;
}

static void title(char *buf, size_t buf_size)
{
    lv_snprintf(buf, (uint32_t)buf_size, "语音给 %s", s_title);
}

const ui_page_def_t ui_page_record = {
    .create  = create,
    .refresh = refresh,
    .prime   = prime,
    .destroy = destroy,
    .title   = title,
    .home    = false,
};
