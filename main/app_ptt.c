/* Push-to-talk, wired into this application.
 *
 * The SDK is ../../wfptt-esp and it is deliberately incomplete: it has the
 * protocol, the channel and the timing, and no microphone. This file is the
 * microphone -- app_audio.c's, already arbitrated against a call and against
 * voice messages -- plus the three other things an application has to decide:
 * what to do with the recording a talk leaves behind, what to say when the
 * channel is refused, and when to repaint.
 *
 * Everything LVGL is next door in ui_ptt.c. Nothing here draws.
 *
 * ------------------------------------------------------------------------
 * Why the audio is lent rather than owned, which is the one thing worth
 * understanding before changing anything here.
 *
 * There is one I2S bus and two codecs on it, and three features want them: a
 * call (wfav), a voice message (app_audio.c) and a talk. Whoever is second
 * has to be told no, and a latch can only do that if all three go through it
 * -- so the latch is app_audio.c's, and both SDKs ask. wfav asks by refusing
 * to start a call while app_audio holds it; wfptt asks through the seven
 * function pointers below, which are app_audio.c's own entry points with the
 * signatures wfptt_audio.h wants.
 *
 * That is why there is no #ifdef anywhere in app_audio.c about push-to-talk.
 * It gained two things -- a way to read a recording while it is still running
 * and a speaker that stays open across chunks -- and neither knows what they
 * are for.
 *
 * ------------------------------------------------------------------------
 * The recording a talk leaves behind.
 *
 * Every press is also a complete .amr, and CONFIG_WFPTT_SAVE_VOICE_MESSAGE
 * hands it here to be sent as an ordinary voice message (content type 23), so
 * the conversation keeps a record of what was said for anyone who was not
 * listening at the time. It goes to ui_voice.c's uploader, which is the same
 * task and the same queue a voice message recorded by hand uses -- there is
 * no second uploader, and a talk does not wait for the last one to finish
 * going up.
 */

#include <string.h>

#include "esp_log.h"

#include "app_audio.h"
#include "ui.h"
#include "ui_page.h"
#include "ui_voice.h"
#include "wfc_mem.h"
#include "wfptt.h"

static const char *TAG = "app_ptt";

/* ------------------------------------------------------------- the audio */

/* Seven one-line adapters. They exist because wfptt_audio_t's functions carry
 * a `ud` and app_audio.c's do not -- app_audio is a singleton, there being one
 * bus. Writing them out beats making app_audio.c take a context it would
 * never read. */

static esp_err_t ptt_record_start(void *ud)
{
    (void)ud;
    return app_audio_record_start();
}

static const uint8_t *ptt_record_data(void *ud, size_t *len)
{
    (void)ud;
    return app_audio_record_body(len);
}

static esp_err_t ptt_record_stop(void *ud, uint8_t **amr, size_t *len, int *seconds)
{
    (void)ud;
    return app_audio_record_stop(amr, len, seconds);
}

static void ptt_record_cancel(void *ud)
{
    (void)ud;
    app_audio_record_cancel();
}

static esp_err_t ptt_play_open(void *ud)
{
    (void)ud;
    return app_audio_play_open();
}

static esp_err_t ptt_play_write(void *ud, const uint8_t *amr, size_t len)
{
    (void)ud;
    return app_audio_play_write(amr, len);
}

static void ptt_play_close(void *ud)
{
    (void)ud;
    app_audio_play_close();
}

/* ------------------------------------------------------------- the events */

/* All five run on the wfptt task, so they obey the rule every other event
 * callback in this application obeys: mark the screen dirty, log a line, and
 * get out. The page reads the state back through wfptt_status() when it
 * repaints, which is the same read model the rest of the client uses -- the
 * event says "something moved", not what to draw. */

static void on_talk_begin(const wfc_conversation_t *conv, void *ud)
{
    (void)conv;
    (void)ud;
    ui_dirty(UI_DIRTY_PTT);
}

static void on_talk_end(const wfc_conversation_t *conv, wfptt_end_reason_t reason,
                        void *ud)
{
    (void)conv;
    (void)ud;

    /* Only the endings a person did not cause are worth a line in the log. A
     * released button explains itself. */
    switch (reason) {
    case WFPTT_END_USER_RELEASE:
        break;
    case WFPTT_END_TIMEOUT:
        ui_log(UI_LOG_NOTE, "说话时间到了");
        break;
    case WFPTT_END_CHANNEL_MUTED:
        ui_log(UI_LOG_ERROR, "这个群已经禁言");
        break;
    case WFPTT_END_MEMBER_MUTED:
        ui_log(UI_LOG_ERROR, "你在这个群里被禁言了");
        break;
    case WFPTT_END_NOT_IN_CHANNEL:
        ui_log(UI_LOG_ERROR, "你已经不在这个群里");
        break;
    case WFPTT_END_MEDIA:
        ui_log(UI_LOG_ERROR, "麦克风没有声音");
        break;
    default:
        ui_logf(UI_LOG_ERROR, "对讲中断：%s", wfptt_end_reason_str(reason));
        break;
    }
    ui_dirty(UI_DIRTY_PTT);
}

static void on_talk_failed(const wfc_conversation_t *conv, int error_code, void *ud)
{
    (void)conv;
    (void)ud;

    switch (error_code) {
    case WFPTT_ERR_OCCUPIED:
    case WFPTT_ERR_MAX_SPEAKER:
        /* The ordinary one, and the reason push-to-talk needs a screen at
         * all: somebody else is holding the channel. */
        ui_log(UI_LOG_NOTE, "对方正在讲话");
        break;
    case WFPTT_ERR_DISCONNECTED:
        ui_log(UI_LOG_ERROR, "未连接，不能对讲");
        break;
    case WFPTT_ERR_RECORDER_ERROR:
        ui_log(UI_LOG_ERROR, "麦克风被占用");
        break;
    case WFPTT_ERR_TALKING:
        break;   /* a double tap; it says so on screen already */
    default:
        ui_logf(UI_LOG_ERROR, "不能讲话：%s", wfptt_error_str(error_code));
        break;
    }
    ui_dirty(UI_DIRTY_PTT);
}

static void on_user_start_talking(const wfc_conversation_t *conv,
                                  const char *user_id, void *ud)
{
    (void)ud;

    char who[WFC_NAME_MAX];

    wfc_get_display_name(user_id,
                         conv->type == WFC_CONV_GROUP ? conv->target : NULL,
                         who, sizeof(who));
    ESP_LOGI(TAG, "%s is talking", who);
    ui_dirty(UI_DIRTY_PTT);
}

static void on_user_end_talking(const wfc_conversation_t *conv,
                                const char *user_id, void *ud)
{
    (void)conv;
    (void)user_id;
    (void)ud;
    ui_dirty(UI_DIRTY_PTT);
}

/* --------------------------------------------------------- the keepsake */

/* Runs on the wfptt task and OWNS `amr`. It parks it on ui_voice.c's queue
 * and returns, which is both halves of what it has to do: the upload is
 * seconds and this task has a channel to keep running, and the queue takes
 * ownership so the 96 KB is freed on every path including the failures. */
static void on_recording(const wfc_conversation_t *conv, uint8_t *amr, size_t len,
                         int seconds, void *ud)
{
    (void)ud;
    ui_voice_send_ptt(conv, amr, len, seconds);
}

/* ------------------------------------------------------------- public API */

bool ui_ptt_available(void)
{
    return true;
}

void ui_ptt_start(void)
{
    wfptt_config_t cfg = {
        .audio = {
            .record_start  = ptt_record_start,
            .record_data   = ptt_record_data,
            .record_stop   = ptt_record_stop,
            .record_cancel = ptt_record_cancel,
            .play_open     = ptt_play_open,
            .play_write    = ptt_play_write,
            .play_close    = ptt_play_close,
        },
        .on_recording = on_recording,
        /* Every board in this deployment is equal. A deployment where some
         * are not -- a dispatcher that can cut in -- raises this, and the
         * listeners drop whoever they are playing for it. */
        .priority = 0,
    };

    esp_err_t err = wfptt_start(&cfg);

    if (err != ESP_OK) {
        /* Never fatal, for the reason calls are never fatal: a board that
         * came up as a chat client is more useful than one that refused to
         * boot. The 对讲 button then reports that it cannot start. */
        ESP_LOGE(TAG, "push-to-talk did not start: %s", esp_err_to_name(err));
        ui_logf(UI_LOG_ERROR, "对讲未能启动：%s", esp_err_to_name(err));
        return;
    }

    wfptt_on_talk_begin(on_talk_begin, NULL);
    wfptt_on_talk_end(on_talk_end, NULL);
    wfptt_on_talk_failed(on_talk_failed, NULL);
    wfptt_on_user_start_talking(on_user_start_talking, NULL);
    wfptt_on_user_end_talking(on_user_end_talking, NULL);
}

bool ui_ptt_busy(void)
{
    wfptt_status_t st;

    wfptt_status(&st);
    return st.state != WFPTT_IDLE || st.speaker[0] != '\0';
}
