/* The two slow halves of a voice message. See ui_voice.h. */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_audio.h"
#include "ui.h"
#include "ui_media.h"
#include "ui_page.h"
#include "ui_voice.h"
#include "wfc_client.h"
#include "wfc_media.h"
#include "wfc_mem.h"

static const char *TAG = "ui_voice";

/* Two is enough to cover "recorded another one while the first was still
 * going up" and small enough that a board with no network does not sit on a
 * megabyte of PSRAM it will never send. A third is refused at the door, where
 * there is still someone to tell. */
#define SEND_QUEUE_LEN 2

/* Both stacks are sized by the TLS session each task holds open, not by the
 * codec: the AMR-NB decoder is the cheap direction (its deepest frame is half
 * a kilobyte, against the encoder's several -- see REC_STACK in app_audio.c),
 * while esp_http_client with the certificate bundle attached is what
 * ui_media.c already gives 8 KB for the download alone. On top of that both
 * of these reach the store, which is SQLite on FAT on wear levelling, and
 * that is not cheap on a stack either.
 *
 * INTERNAL SRAM, and that is the whole point of this comment.
 *
 * They were in PSRAM for one build, on the reasoning that the recorder's
 * stack is there and internal SRAM is the scarce pool. The board answered:
 *
 *   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
 *   cache_utils.c:114 (esp_task_stack_is_sane_cache_disabled())
 *
 * on the first voice message that actually got sent -- inside
 * wfc_clear_unread(), through SQLite, FATFS, wear levelling and finally
 * esp_flash_read(), which turns the flash cache OFF for the length of the
 * read. A task whose stack lives in PSRAM cannot be running while that
 * happens, because its stack is on the far side of the cache that was just
 * disabled; IDF asserts rather than letting it fault.
 *
 * So the rule this file now follows, and the one wfav_audio.c has been
 * following by luck: a PSRAM stack is for a task that touches the network and
 * a codec and NOTHING THAT READS FLASH. Reaching wfc_client.h at all -- one
 * message read, one unread count cleared -- disqualifies a task from it. */
#define PLAY_STACK (12 * 1024)
#define SEND_STACK (12 * 1024)

typedef struct {
    wfc_conversation_t conv;
    uint8_t           *amr;
    size_t             len;
    int                seconds;
    /* WFC_CONTENT_VOICE, or the push-to-talk flavour of it. The upload is the
     * same file to the same place either way -- only the type number and the
     * one line the conversation list shows differ. */
    int32_t            type;
} job_t;

/* SoundMessageContent with a different number on it: what ptt.js and
 * android-pttclient send at the end of a talk. Spelled out here rather than
 * taken from wfptt_types.h because this file is compiled in every build,
 * including the ones with no PTT SDK -- where a talk's voice message still
 * arrives from other people and still has to be sent nowhere and played
 * normally. */
#define CONTENT_PTT_SOUND 23

static SemaphoreHandle_t s_play_work;
static QueueHandle_t     s_send_queue;
static volatile int64_t  s_want;
static volatile int64_t  s_playing;

/* -------------------------------------------------------------- playing */

static void play_one(int64_t uid)
{
    char *url = ui_media_url(uid);

    if (url == NULL) {
        ESP_LOGW(TAG, "message %lld has no audio to play", (long long)uid);
        return;
    }

    size_t   len = 0;
    uint8_t *amr = ui_media_download(url, &len);

    wfc_free(url);
    if (amr == NULL) {
        ui_log(UI_LOG_ERROR, "语音下载失败");
        return;
    }

    /* Marked as playing only once there is something to play, so a bubble
     * does not show a stop symbol through a download that may fail. */
    s_playing = uid;
    ui_dirty(UI_DIRTY_MESSAGES);

    app_audio_play(amr, len);
    wfc_free(amr);

    s_playing = 0;
    ui_dirty(UI_DIRTY_MESSAGES);
}

static void play_task(void *arg)
{
    (void)arg;

    while (true) {
        xSemaphoreTake(s_play_work, portMAX_DELAY);

        int64_t uid = s_want;

        s_want = 0;
        if (uid != 0) {
            play_one(uid);
            ESP_LOGI(TAG, "player peak stack: %u of %u bytes",
                     (unsigned)(PLAY_STACK - uxTaskGetStackHighWaterMark(NULL)),
                     (unsigned)PLAY_STACK);
        }
    }
}

void ui_voice_play(int64_t message_uid)
{
    if (s_play_work == NULL || message_uid == 0) {
        return;
    }
    s_want = message_uid;
    xSemaphoreGive(s_play_work);
}

bool ui_voice_playing(void)
{
    return app_audio_playing();
}

int64_t ui_voice_playing_uid(void)
{
    return s_playing;
}

void ui_voice_stop(void)
{
    app_audio_stop_playing();
}

/* -------------------------------------------------------------- sending */

static void send_one(job_t *job)
{
    char url[WFC_MEDIA_URL_MAX];

    /* Two round trips, and the first is the one that fails on a deployment
     * this client has not met: wfc_media.c refuses the two upload backends
     * that encrypt the body rather than sending something unplayable. */
    esp_err_t err = wfc_media_upload(WFC_MEDIA_VOICE, ".amr", NULL, job->amr,
                                     job->len, url, sizeof(url));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice upload failed: %s", esp_err_to_name(err));
        ui_logf(UI_LOG_ERROR, "语音上传失败: %s", esp_err_to_name(err));
        return;
    }

    /* Seconds, not milliseconds -- soundMessageContent.js writes seconds and
     * every client reads them that way. */
    char content[32];

    snprintf(content, sizeof(content), "{\"duration\":%d}", job->seconds);

    wfc_content_out_t out = {
        .type             = job->type,
        .persist_flag     = WFC_PERSIST_FROM_TYPE,
        .media_type       = WFC_MEDIA_VOICE,
        .remote_media_url = url,
        .content          = content,
        /* What a client that cannot draw the type shows, and what a push
         * notification says. The reference clients both use these exact
         * strings -- pttSoundMessageContent.js digests to 「对讲语音」. */
        .searchable_content = job->type == CONTENT_PTT_SOUND ? "[对讲语音]" : "[语音]",
    };

    err = wfc_send_message(&job->conv, &out, NULL, 0, NULL, NULL);
    if (err != ESP_OK) {
        ui_logf(UI_LOG_ERROR, "语音发送未能提交: %s", esp_err_to_name(err));
        return;
    }

    /* Same as the chat page does after a text message: answering a
     * conversation is reading it. */
    wfc_clear_unread(&job->conv);
}

static void send_task(void *arg)
{
    (void)arg;

    while (true) {
        job_t job;

        if (xQueueReceive(s_send_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        send_one(&job);

        /* Here and nowhere else, so every path out of send_one() -- and there
         * are four -- releases the recorder's 96 KB. */
        wfc_free(job.amr);

        ESP_LOGI(TAG, "sender peak stack: %u of %u bytes",
                 (unsigned)(SEND_STACK - uxTaskGetStackHighWaterMark(NULL)),
                 (unsigned)SEND_STACK);
    }
}

static void enqueue(const wfc_conversation_t *conv, uint8_t *amr, size_t len,
                    int seconds, int32_t type)
{
    if (amr == NULL) {
        return;
    }
    if (s_send_queue == NULL || conv == NULL) {
        wfc_free(amr);
        return;
    }

    job_t job = {
        .amr     = amr,
        .len     = len,
        .seconds = seconds,
        .type    = type,
    };

    job.conv = *conv;

    if (xQueueSend(s_send_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "the send queue is full; dropping a %d s message", seconds);
        ui_log(UI_LOG_ERROR, "还有语音在上传，请稍后再试");
        wfc_free(amr);
    }
}

void ui_voice_send(const wfc_conversation_t *conv, uint8_t *amr, size_t len,
                   int seconds)
{
    enqueue(conv, amr, len, seconds, WFC_CONTENT_VOICE);
}

void ui_voice_send_ptt(const wfc_conversation_t *conv, uint8_t *amr, size_t len,
                       int seconds)
{
    enqueue(conv, amr, len, seconds, CONTENT_PTT_SOUND);
}

/* --------------------------------------------------------------- startup */

void ui_voice_start(void)
{
    if (s_play_work != NULL) {
        return;
    }

    s_play_work  = xSemaphoreCreateBinary();
    s_send_queue = xQueueCreate(SEND_QUEUE_LEN, sizeof(job_t));

    if (s_play_work == NULL || s_send_queue == NULL) {
        ESP_LOGE(TAG, "no room for the voice mailboxes");
        return;
    }

    if (xTaskCreate(play_task, "voice_play", PLAY_STACK, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(send_task, "voice_send", SEND_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no room for the voice tasks; voice messages will not work");
    }
}
