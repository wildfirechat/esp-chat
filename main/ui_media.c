/* Fetching and decoding pictures. See ui_media.h for the contract; this file
 * is the three things behind it -- a fixed table of slots, one task, and the
 * lock between them.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "jpeg_decoder.h"

#include "ui_media.h"
#include "ui_page.h"

#include "wfc_client.h"
#include "wfc_mem.h"

static const char *TAG = "ui_media";

/* How many pictures are held at once. A 320x240 panel shows two or three
 * bubbles, so this is "the ones you can see, plus the next scroll", not a
 * history -- and every one of them is ~26 KB of PSRAM. */
#define SLOTS 4

/* The most we will pull down for one picture. remoteMediaUrl is the FULL
 * image, and a phone camera's idea of that is measured in megabytes; past
 * this the frame stays empty, which is the honest outcome for a board that
 * cannot show the detail anyway. */
#define MAX_BYTES (768 * 1024)

/* TJpgDec's scratchpad. 3.1 KB is the library's own recommendation and 4 KB
 * is what LVGL's copy uses; it is not a tuning knob. */
#define WORK_BYTES 4096

#define HTTP_TIMEOUT_MS 15000

/* Enough for an object store handing out a signed URL, few enough that a
 * redirect loop ends. */
#define MAX_REDIRECTS 4

/* Internal SRAM, and this task holds a TLS session while it runs, so it is
 * not a place to be stingy -- but see ui_media.h on when it is created. */
#define TASK_STACK 8192

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_WANTED,    /* a view asked; the fetcher has not started */
    SLOT_LOADING,   /* the fetcher owns this slot */
    SLOT_READY,
    SLOT_FAILED,    /* asked and could not be had; do not ask again */
} slot_state_t;

typedef struct {
    int64_t        uid;
    slot_state_t   state;
    uint8_t       *pixels;   /* RGB565, wfc_malloc'd; NULL unless READY */
    lv_image_dsc_t dsc;
} slot_t;

static slot_t            s_slots[SLOTS];
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_work;    /* given when something becomes WANTED */
static TaskHandle_t      s_task;

/* ------------------------------------------------------------- the fetch */

/* Runs inside wfc_get_message(), where the message is alive and the store's
 * lock is held: copy and get out. The copy is on the heap rather than into a
 * fixed buffer because the store keeps a URL up to
 * CONFIG_WFC_STORE_MAX_TEXT and clipping one is a picture that never
 * loads. */
static bool take_url(const wfc_message_t *msg, void *ud)
{
    char **out = ud;

    if (msg->content.remote_media_url != NULL &&
        msg->content.remote_media_url[0] != '\0') {
        *out = wfc_strdup(msg->content.remote_media_url);
    }
    return false;
}

/* The whole body, in PSRAM, or NULL.
 *
 * A response with no Content-Length is refused rather than read into a
 * growing buffer: this runs with a TLS session open on a board whose internal
 * heap is the scarce one, and "how big is it" is a question worth having
 * answered before committing. Every file server WFC ships with answers it. */
uint8_t *ui_media_download(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https, when it is */
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);

    if (http == NULL) {
        return NULL;
    }

    uint8_t *body   = NULL;
    int64_t  length = 0;

    /* Followed by hand, because opening the connection ourselves -- which is
     * what lets the body go straight into one PSRAM buffer -- is the one mode
     * esp_http_client does not redirect for. A signed URL on an object store
     * is usually a redirect or two. */
    for (int hop = 0; ; hop++) {
        if (esp_http_client_open(http, 0) != ESP_OK) {
            ESP_LOGW(TAG, "cannot open %s", url);
            goto done;
        }

        length      = esp_http_client_fetch_headers(http);
        int status  = esp_http_client_get_status_code(http);

        if (status == 200) {
            break;
        }
        if (hop >= MAX_REDIRECTS || esp_http_client_set_redirection(http) != ESP_OK) {
            ESP_LOGW(TAG, "HTTP %d for %s", status, url);
            goto done;
        }
        esp_http_client_close(http);
    }
    if (length <= 0 || length > MAX_BYTES) {
        ESP_LOGW(TAG, "%s is %lld bytes, cap is %d", url, (long long)length, MAX_BYTES);
        goto done;
    }

    body = wfc_malloc((size_t)length);
    if (body == NULL) {
        ESP_LOGW(TAG, "no room for %lld bytes", (long long)length);
        goto done;
    }

    /* Read to the end rather than trusting one call: esp_http_client_read()
     * returns what one socket read gave it. */
    int got = 0;

    while (got < (int)length) {
        int n = esp_http_client_read(http, (char *)body + got, (int)length - got);

        if (n <= 0) {
            ESP_LOGW(TAG, "short read: %d of %lld", got, (long long)length);
            wfc_free(body);
            body = NULL;
            goto done;
        }
        got += n;
    }
    *out_len = (size_t)length;

done:
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    return body;
}

/* JPEG in, RGB565 out, scaled down to fit UI_MEDIA_BOX during the decode
 * rather than after it -- TJpgDec's 1/2, 1/4 and 1/8 are free, and decoding a
 * 12 megapixel photo at full size would want 24 MB. */
static uint8_t *decode(uint8_t *jpeg, size_t len, lv_image_dsc_t *dsc)
{
    uint8_t *work = wfc_malloc(WORK_BYTES);

    if (work == NULL) {
        return NULL;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata      = jpeg,
        .indata_size = (uint32_t)len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
        .advanced    = { .working_buffer = work, .working_buffer_size = WORK_BYTES },
    };
    esp_jpeg_image_output_t info = { 0 };
    uint8_t                *pixels = NULL;

    /* info.width/height come back as the SOURCE size whatever the scale is
     * asked for -- only output_len is scaled -- so the fit is worked out from
     * the divisor rather than from what it reports. */
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        ESP_LOGW(TAG, "not a JPEG this decoder understands");
        goto done;
    }

    for (int scale = JPEG_IMAGE_SCALE_0; scale <= JPEG_IMAGE_SCALE_1_8; scale++) {
        int div = 1 << scale;

        cfg.out_scale = (esp_jpeg_image_scale_t)scale;
        if ((info.width / div <= UI_MEDIA_BOX_W && info.height / div <= UI_MEDIA_BOX_H) ||
            scale == JPEG_IMAGE_SCALE_1_8) {
            break;
        }
    }

    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        goto done;
    }

    pixels = wfc_malloc(info.output_len);
    if (pixels == NULL) {
        ESP_LOGW(TAG, "no room for %u decoded bytes", (unsigned)info.output_len);
        goto done;
    }

    cfg.outbuf      = pixels;
    cfg.outbuf_size = info.output_len;

    /* Now info carries the SCALED size, which is what the widget is drawn
     * at. A truncated or malformed JPEG fails here rather than half-way into
     * the buffer, so a failure costs a frame and never a wrong picture. */
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK) {
        ESP_LOGW(TAG, "decode failed");
        wfc_free(pixels);
        pixels = NULL;
        goto done;
    }

    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic     = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf        = LV_COLOR_FORMAT_RGB565;
    dsc->header.w         = info.width;
    dsc->header.h         = info.height;
    dsc->header.stride    = (uint32_t)info.width * 2;
    dsc->data_size        = info.output_len;
    /* dsc->data is set by the caller, once the buffer belongs to a slot. */

done:
    wfc_free(work);
    return pixels;
}

/* --------------------------------------------------------------- the task */

static bool claim(int64_t *uid)
{
    bool claimed = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < SLOTS; i++) {
        if (s_slots[i].state == SLOT_WANTED) {
            s_slots[i].state = SLOT_LOADING;
            *uid             = s_slots[i].uid;
            claimed          = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return claimed;
}

/* Hands the result back to the slot the fetcher claimed. The slot cannot have
 * been reclaimed underneath us -- ui_media_keep_only() leaves LOADING alone --
 * but it is checked anyway, because the alternative to checking is a leak
 * nobody would ever see. */
static void settle(int64_t uid, uint8_t *pixels, const lv_image_dsc_t *dsc)
{
    bool placed = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < SLOTS; i++) {
        if (s_slots[i].state != SLOT_LOADING || s_slots[i].uid != uid) {
            continue;
        }
        if (pixels != NULL) {
            s_slots[i].pixels   = pixels;
            s_slots[i].dsc      = *dsc;
            s_slots[i].dsc.data = pixels;
            s_slots[i].state    = SLOT_READY;
            placed              = true;
        } else {
            s_slots[i].state = SLOT_FAILED;
        }
        break;
    }
    xSemaphoreGive(s_lock);

    if (pixels != NULL && !placed) {
        wfc_free(pixels);
    }
}

char *ui_media_url(int64_t message_uid)
{
    char *url = NULL;

    if (!wfc_get_message(message_uid, take_url, &url)) {
        return NULL;
    }
    return url;
}

static void fetch(int64_t uid)
{
    char *url = ui_media_url(uid);

    if (url == NULL) {
        ESP_LOGW(TAG, "message %lld has no picture to fetch", (long long)uid);
        settle(uid, NULL, NULL);
        return;
    }

    size_t   len  = 0;
    uint8_t *jpeg = ui_media_download(url, &len);

    wfc_free(url);
    if (jpeg == NULL) {
        settle(uid, NULL, NULL);
        return;
    }

    lv_image_dsc_t dsc;
    uint8_t       *pixels = decode(jpeg, len, &dsc);

    wfc_free(jpeg);
    settle(uid, pixels, &dsc);
}

static void media_task(void *arg)
{
    (void)arg;

    while (true) {
        xSemaphoreTake(s_work, portMAX_DELAY);

        int64_t uid = 0;

        /* One at a time and to exhaustion: the semaphore only says "there was
         * work", so the loop is what makes two asks in one repaint into two
         * fetches. */
        while (claim(&uid)) {
            fetch(uid);
            /* Off the lock, and the page's own dirty rules decide what that
             * repaint does -- a picture landing must not scroll the view the
             * way an arriving message does. */
            ui_dirty(UI_DIRTY_MEDIA);
        }
    }
}

/* ------------------------------------------------------------ the seams */

void ui_media_start(void)
{
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_work = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_work == NULL) {
        ESP_LOGE(TAG, "no room for the picture fetcher; pictures stay blank");
    }
}

const lv_image_dsc_t *ui_media_get(int64_t message_uid)
{
    if (s_lock == NULL || s_work == NULL || message_uid <= 0) {
        return NULL;
    }

    /* Never wait. The fetcher holds this lock only to move a slot between
     * states -- never across the download -- so losing the race means one
     * repaint draws the frame again, and the next one does not. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return NULL;
    }

    const lv_image_dsc_t *ready = NULL;
    slot_t               *spare = NULL;
    bool                  known = false;

    for (size_t i = 0; i < SLOTS; i++) {
        if (s_slots[i].state != SLOT_EMPTY && s_slots[i].uid == message_uid) {
            known = true;
            if (s_slots[i].state == SLOT_READY) {
                ready = &s_slots[i].dsc;
            }
            break;
        }
        if (s_slots[i].state == SLOT_EMPTY && spare == NULL) {
            spare = &s_slots[i];
        }
    }

    /* Full and this one is not held: nothing to do. The slots are freed by
     * ui_media_keep_only() as the conversation scrolls, so this resolves
     * itself rather than needing an eviction policy that could pull a picture
     * out from under a widget. */
    bool wanted = !known && spare != NULL;

    if (wanted) {
        spare->uid   = message_uid;
        spare->state = SLOT_WANTED;
    }
    xSemaphoreGive(s_lock);

    if (wanted) {
        /* The first picture anyone looks at is what creates the task; see
         * ui_media.h. Failing to create it leaves the slot WANTED, which is
         * drawn as a frame -- the same as a picture that has not arrived. */
        if (s_task == NULL &&
            xTaskCreate(media_task, "media", TASK_STACK, NULL, 3, &s_task) != pdPASS) {
            s_task = NULL;
            ESP_LOGE(TAG, "no room for the picture fetcher; pictures stay blank");
            return NULL;
        }
        xSemaphoreGive(s_work);
    }
    return ready;
}

void ui_media_keep_only(const int64_t *uids, size_t n)
{
    if (s_lock == NULL) {
        return;
    }
    /* A short wait rather than none: this is the only place memory is given
     * back, and skipping it means a conversation full of pictures stops
     * loading new ones. The fetcher never holds the lock for long. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    for (size_t i = 0; i < SLOTS; i++) {
        /* LOADING belongs to the fetcher, which is off the lock and about to
         * write into it. */
        if (s_slots[i].state == SLOT_EMPTY || s_slots[i].state == SLOT_LOADING) {
            continue;
        }

        bool keep = false;

        for (size_t j = 0; j < n && !keep; j++) {
            keep = uids[j] == s_slots[i].uid;
        }
        if (keep) {
            continue;
        }

        wfc_free(s_slots[i].pixels);
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
    }
    xSemaphoreGive(s_lock);
}
