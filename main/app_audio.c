/* AMR-NB in and out of the board's two codecs. See app_audio.h. */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/esp-box-3.h"
#include "esp_amrnb_dec.h"
#include "esp_amrnb_enc.h"
#include "esp_codec_dev.h"

#include "app_audio.h"
#include "ui_page.h"
#include "wfc_mem.h"

static const char *TAG = "app_audio";

/* AMR-NB is 8 kHz mono by definition; the encoder rejects anything else. */
#define SAMPLE_RATE 8000
#define CHANNELS    1

/* The ES7210's PGA runs at 0 dB unless somebody sets it, and 0 dB on this
 * board's analogue MEMS microphones is close enough to the noise floor that a
 * recording carries nothing -- while every step of the pipeline reports
 * success. This is the same number and the same reason as
 * CONFIG_WFAV_MIC_GAIN_DB in the AV SDK (wfav_board_esp_box_3.c), repeated
 * here rather than shared because a build with CONFIG_APP_CALL=n has no AV
 * SDK to take it from. */
#define MIC_GAIN_DB 37.5f

/* One AMR-NB frame is 20 ms whatever the bitrate, so this is the clock
 * everything else is counted in. */
#define FRAME_MS 20

/* The recorder's stack, and it is not a round number picked for comfort.
 *
 * The AMR-NB encoder in esp_audio_codec is the ETSI fixed-point reference,
 * and that code works on its CALLER's stack rather than out of the state it
 * was opened with: cod_amr() opens a 1.4 KB frame, the fixed codebook search
 * under it (code_10i40_35bits) another 3.6 KB, and the open-loop pitch search
 * beside it 1.3 KB -- with the codec driver's read on top of all of it. 4 KB
 * was not enough and the board goes down with
 *
 *   ***ERROR*** A stack overflow in task voice_rec has been detected.
 *
 * on the first frame it encodes. Which is the same failure, from the same
 * cause, that wfav_audio.c's player hit at 5 KB, and the component's own
 * README says the same thing from the other end: a task running these
 * encoders wants about 40 KB to be safe with all of them.
 *
 * In PSRAM for the reason that file gives too -- internal SRAM is the pool
 * that decides whether anything else on this board can start, and this task
 * never reads flash: an I2S read and a fixed-point codec, start to finish.
 *
 * That last clause is a CONDITION, not an observation. A task whose stack is
 * in PSRAM cannot be running when the flash cache is turned off, and any read
 * that reaches the store turns it off -- SQLite, FATFS, wear levelling,
 * esp_flash_read(). ui_voice.c has the assert this earns, from the one build
 * where its two tasks were in PSRAM. Anything added below that wants a
 * message, a conversation or a setting has to move this stack to internal
 * SRAM in the same commit.
 *
 * The task reports what it actually used on the way out, so whatever replaces
 * this number is measured rather than guessed. */
#define REC_STACK (24 * 1024)
#define REC_CAPS  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* The top of AMR-NB's range: 12.2 kbps, 32 bytes for each 20 ms. Voice
 * messages are short, and the board has budget for the bytes far more than it
 * has patience for a message that sounds poor. */
#define BITRATE       ESP_AMRNB_ENC_BITRATE_MR122
#define MAX_FRAME_LEN 32

/* "#!AMR\n" -- the magic every decoder looks for, and the reason
 * no_file_header stays false in the encoder config. A .amr without it plays
 * nowhere. */
#define AMR_HEADER     "#!AMR\n"
#define AMR_HEADER_LEN 6

/* Data bytes per frame for each of the sixteen mode values in a frame
 * header's bits 3..6. Modes 9..15 are comfort noise or reserved and do not
 * occur in a file this board records, but a file it PLAYS came from somewhere
 * else -- they are here to be stopped on rather than trusted. */
static const uint8_t FRAME_BYTES[16] = {
    12, 13, 15, 17, 19, 20, 26, 31, 5, 0, 0, 0, 0, 0, 0, 0
};

/* One at a time: recording, playing, and (via ui_call_busy) a call.
 *
 * A binary semaphore rather than a mutex, and that is deliberate. Recording
 * takes it in app_audio_record_start() and releases it in _stop(), which is
 * not guaranteed to be the same task -- a page may well start from a button
 * and stop from its prime(). FreeRTOS mutexes have an owner and refuse a give
 * from anyone else; a binary semaphore has no owner and is what "held across
 * two calls" actually needs. */
static SemaphoreHandle_t s_free;

/* Created once and kept. bsp_audio_codec_*_init() builds a fresh I2C control
 * interface and codec interface every time it is called, and esp_codec_dev
 * has no matching teardown for them, so calling it per recording leaks a
 * little each time. Opening and closing the DEVICE is the per-use half, and
 * that is below. */
static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;

/* Guards the handoff of the recording between the recorder task and whoever
 * ends it. Not the same thing as s_free: that one says "the audio path is
 * busy" and is held for the whole recording, this one is held for a few
 * instructions and exists so that "is the task still running, and who frees
 * the buffer" is one decision rather than two racing ones. */
static SemaphoreHandle_t s_state;

static struct {
    TaskHandle_t  task;
    volatile bool stop;
    volatile bool running;
    volatile int  frames;
    /* Set by a cancel that could not wait: the recorder task frees the buffer
     * and releases s_free on its way out instead of the caller doing it. */
    bool          abandoned;
    bool          claimed;    /* we hold s_free */
    uint8_t      *buf;
    /* Read from another task while the recorder appends to it -- push-to-talk
     * polls it every 400 ms for the bytes it has not published yet. It only
     * ever grows, and the frame it counts is already written when it moves,
     * so the poller needs no lock; what it does need is for the value to be
     * re-read rather than kept in a register. */
    volatile size_t len;
    size_t        cap;
} s_rec;

static volatile bool s_playing;
static volatile bool s_stop_play;

void app_audio_init(void)
{
    if (s_free == NULL) {
        s_free = xSemaphoreCreateBinary();
        if (s_free != NULL) {
            xSemaphoreGive(s_free);   /* starts free */
        }
    }
    if (s_state == NULL) {
        s_state = xSemaphoreCreateMutex();
    }
}

static bool claim(void)
{
    return s_free != NULL && s_state != NULL && xSemaphoreTake(s_free, 0) == pdTRUE;
}

static void release(void)
{
    xSemaphoreGive(s_free);
}

static void lock_state(void)   { xSemaphoreTake(s_state, portMAX_DELAY); }
static void unlock_state(void) { xSemaphoreGive(s_state); }

/* Frees the recording and puts the audio path back. Callers hold s_state. */
static void drop_recording(void)
{
    wfc_free(s_rec.buf);
    s_rec.buf = NULL;
    s_rec.len = 0;
    if (s_rec.claimed) {
        s_rec.claimed = false;
        release();
    }
}

/* --------------------------------------------------------------- walking */

int app_audio_amr_seconds(const uint8_t *amr, size_t len)
{
    if (amr == NULL || len <= AMR_HEADER_LEN) {
        return 0;
    }

    size_t i      = memcmp(amr, AMR_HEADER, AMR_HEADER_LEN) == 0 ? AMR_HEADER_LEN : 0;
    int    frames = 0;

    while (i < len) {
        uint8_t size = FRAME_BYTES[(amr[i] >> 3) & 0x0f];

        if (size == 0 || i + (size_t)size + 1 > len) {
            break;
        }
        i += (size_t)size + 1;
        frames++;
    }

    /* Rounded up: a message is never shown as lasting zero seconds. */
    return (frames * FRAME_MS + 999) / 1000;
}

/* ------------------------------------------------------------- recording */

static void record_task(void *arg)
{
    (void)arg;

    void    *enc = NULL;
    int16_t *pcm = NULL;
    bool     open_ok = false;

    if (s_mic == NULL) {
        s_mic = bsp_audio_codec_microphone_init();
    }
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "the microphone codec would not initialise");
        goto done;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = SAMPLE_RATE,
        .channel         = CHANNELS,
        .bits_per_sample = 16,
    };

    if (esp_codec_dev_open(s_mic, &fs) != 0) {
        ESP_LOGE(TAG, "the microphone would not open at %d Hz mono", SAMPLE_RATE);
        goto done;
    }
    open_ok = true;

    /* After the open, not before: the gain register cannot be written while
     * the device is closed. */
    int rc = esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);

    if (rc != 0) {
        ESP_LOGW(TAG, "microphone gain was refused (%d); the recording may be silent", rc);
    }

    esp_amrnb_enc_config_t cfg = ESP_AMRNB_ENC_CONFIG_DEFAULT();

    cfg.bitrate_mode = BITRATE;

    if (esp_amrnb_enc_open(&cfg, sizeof(cfg), &enc) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "the AMR-NB encoder would not open");
        goto done;
    }

    int in_size  = 0;
    int out_size = 0;

    if (esp_amrnb_enc_get_frame_size(enc, &in_size, &out_size) != ESP_AUDIO_ERR_OK ||
        in_size <= 0 || out_size <= 0) {
        ESP_LOGE(TAG, "the encoder would not say its frame size");
        goto done;
    }

    pcm = wfc_malloc((size_t)in_size);
    if (pcm == NULL) {
        goto done;
    }

    ESP_LOGI(TAG, "recording: %d bytes of PCM -> %d bytes of AMR every %d ms",
             in_size, out_size, FRAME_MS);

    /* The magic goes in ahead of the first frame; the encoder emits it with
     * the first frame it produces, so the room for it is already in `cap`. */
    while (!s_rec.stop &&
           s_rec.len + (size_t)out_size + AMR_HEADER_LEN <= s_rec.cap) {
        if (esp_codec_dev_read(s_mic, pcm, (uint32_t)in_size) != 0) {
            ESP_LOGE(TAG, "the microphone stopped delivering");
            break;
        }

        esp_audio_enc_in_frame_t in = {
            .buffer = (uint8_t *)pcm,
            .len    = (uint32_t)in_size,
        };
        esp_audio_enc_out_frame_t out = {
            .buffer = s_rec.buf + s_rec.len,
            .len    = (uint32_t)(s_rec.cap - s_rec.len),
        };

        if (esp_amrnb_enc_process(enc, &in, &out) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "the encoder gave up");
            break;
        }
        s_rec.len += out.encoded_bytes;
        s_rec.frames++;
    }

done:
    if (enc != NULL) {
        esp_amrnb_enc_close(enc);
    }
    if (open_ok) {
        esp_codec_dev_close(s_mic);
    }
    wfc_free(pcm);

    /* Clearing `running` and acting on `abandoned` are one decision, taken
     * under s_state so a cancel arriving right now either wins (and this
     * frees) or loses (and it frees). Split in two, both could conclude the
     * other was going to do it. */
    lock_state();
    s_rec.task    = NULL;
    s_rec.running = false;
    if (s_rec.abandoned) {
        s_rec.abandoned = false;
        drop_recording();
    }
    unlock_state();

    /* Left as a measurement rather than as a guess, like the player's. */
    ESP_LOGI(TAG, "recorder peak stack: %u of %u bytes",
             (unsigned)(REC_STACK - uxTaskGetStackHighWaterMark(NULL)),
             (unsigned)REC_STACK);

    /* Created WithCaps, so it has to be deleted WithCaps or the PSRAM stack
     * leaks -- one recording's worth every recording. */
    vTaskDeleteWithCaps(NULL);
}

esp_err_t app_audio_record_start(void)
{
    app_audio_init();

    if (ui_call_busy()) {
        ESP_LOGW(TAG, "not recording: a call has the audio path");
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The whole message up front rather than a buffer that grows: the cap is
     * what makes it a bounded amount of PSRAM, and a realloc partway through
     * a recording is a gap in the audio. */
    s_rec.cap = (size_t)APP_AUDIO_MAX_SECONDS * (1000 / FRAME_MS) * MAX_FRAME_LEN +
                AMR_HEADER_LEN;
    s_rec.buf = wfc_malloc(s_rec.cap);
    if (s_rec.buf == NULL) {
        release();
        return ESP_ERR_NO_MEM;
    }

    s_rec.len       = 0;
    s_rec.frames    = 0;
    s_rec.stop      = false;
    s_rec.abandoned = false;
    s_rec.claimed   = true;
    s_rec.running   = true;

    if (xTaskCreateWithCaps(record_task, "voice_rec", REC_STACK, NULL, 5,
                            &s_rec.task, REC_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "no room for the recorder task");
        s_rec.running = false;
        lock_state();
        drop_recording();
        unlock_state();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_audio_recording(void)
{
    return s_rec.running;
}

int app_audio_record_seconds(void)
{
    return s_rec.frames * FRAME_MS / 1000;
}

/* Waits for the recorder task to put the microphone down. It checks `stop`
 * once per frame, so this is one read's worth of wait -- the ceiling is only
 * there so a microphone that has stopped delivering cannot hang the caller. */
static void wait_for_recorder(void)
{
    s_rec.stop = true;
    for (int i = 0; i < 100 && s_rec.running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_rec.running) {
        ESP_LOGE(TAG, "the recorder task did not stop");
    }
}

const uint8_t *app_audio_record_body(size_t *len)
{
    if (len == NULL) {
        return NULL;
    }
    *len = 0;

    uint8_t *buf  = s_rec.buf;
    size_t   have = s_rec.len;

    if (buf == NULL || have <= AMR_HEADER_LEN) {
        return NULL;
    }

    /* The encoder emits the magic with the first frame it produces, so it is
     * at the front of this buffer and not in the caller's business. Tested
     * for rather than assumed: no_file_header is a setting, and a build that
     * turned it on would otherwise lose its first frame here. */
    size_t off = memcmp(buf, AMR_HEADER, AMR_HEADER_LEN) == 0 ? AMR_HEADER_LEN : 0;

    *len = have - off;
    return buf + off;
}

esp_err_t app_audio_record_stop(uint8_t **amr, size_t *len, int *seconds)
{
    if (amr == NULL || len == NULL || seconds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rec.buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    wait_for_recorder();

    int frames = s_rec.frames;

    /* Under a second is a mis-tap far more often than it is a message, and
     * the far end draws a bubble either way. */
    if (frames * FRAME_MS < 1000) {
        ESP_LOGI(TAG, "discarding a %d ms recording", frames * FRAME_MS);
        lock_state();
        drop_recording();
        unlock_state();
        return ESP_ERR_INVALID_SIZE;
    }

    *amr     = s_rec.buf;
    *len     = s_rec.len;
    *seconds = (frames * FRAME_MS + 999) / 1000;

    lock_state();
    s_rec.buf = NULL;   /* the caller owns it now */
    drop_recording();   /* nothing left to free; this is here for the latch */
    unlock_state();

    ESP_LOGI(TAG, "recorded %d s, %u bytes", *seconds, (unsigned)*len);
    return ESP_OK;
}

void app_audio_record_cancel(void)
{
    if (s_state == NULL) {
        return;
    }

    /* Asked for either way; what differs is who cleans up. */
    s_rec.stop = true;

    lock_state();
    if (s_rec.running) {
        /* Do NOT wait. The one caller that matters is the record page's
         * destroy(), which runs on the UI task with the display lock held --
         * waiting there for a microphone to be put down is the freeze
         * ui_media.h describes. The recorder task frees the buffer and
         * releases the audio path when it notices `stop`, which is within one
         * frame. */
        s_rec.abandoned = true;
    } else {
        drop_recording();
    }
    unlock_state();
}

/* --------------------------------------------------------------- playing */

/* Two callers now: a voice message, which has the whole file and plays it in
 * one call, and push-to-talk, which is handed 400 ms at a time for as long as
 * somebody keeps talking. They differ in exactly one thing -- whether the
 * speaker is opened per call or held open across many -- so what follows is
 * the four steps of a play written once, and two entry points that arrange
 * them differently.
 *
 * The latch is taken by whoever opens the speaker and released by whoever
 * closes it, in both shapes. That is what keeps "a call, a recording, a
 * message and a talk cannot use the codec at the same time" one rule rather
 * than four. */

static esp_err_t open_speaker(void)
{
    if (s_spk == NULL) {
        s_spk = bsp_audio_codec_speaker_init();
    }
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "the speaker codec would not initialise");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = SAMPLE_RATE,
        .channel         = CHANNELS,
        .bits_per_sample = 16,
    };

    if (esp_codec_dev_open(s_spk, &fs) != 0) {
        ESP_LOGE(TAG, "the speaker would not open at %d Hz mono", SAMPLE_RATE);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, CONFIG_APP_VOICE_VOLUME);
    return ESP_OK;
}

/* 160 samples per frame at 8 kHz; twice that is room to spare for a decoder
 * that decides to hand back two at once. */
#define PCM_BYTES (160 * 2 * 2)

/* Walks the frames and writes them out. `stop` may be NULL; when it is not,
 * it is checked between frames so a play can be cut short.
 *
 * Skips a leading file header if there is one. A stream's chunks do not carry
 * it and a file's does, and this is where both are true at once. */
static void decode_frames(void *dec, int16_t *pcm, const uint8_t *amr, size_t len,
                          const volatile bool *stop)
{
    size_t i = len > AMR_HEADER_LEN && memcmp(amr, AMR_HEADER, AMR_HEADER_LEN) == 0
                   ? AMR_HEADER_LEN
                   : 0;

    while (i < len && (stop == NULL || !*stop)) {
        uint8_t size = FRAME_BYTES[(amr[i] >> 3) & 0x0f];

        if (size == 0 || i + (size_t)size + 1 > len) {
            break;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer = (uint8_t *)(amr + i),
            .len    = (uint32_t)size + 1,
        };
        esp_audio_dec_out_frame_t out = {
            .buffer = (uint8_t *)pcm,
            .len    = (uint32_t)PCM_BYTES,
        };
        esp_audio_dec_info_t info = { 0 };

        if (esp_amrnb_dec_decode(dec, &raw, &out, &info) != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "a frame would not decode; stopping here");
            break;
        }
        if (out.decoded_size > 0 &&
            esp_codec_dev_write(s_spk, pcm, out.decoded_size) != 0) {
            ESP_LOGE(TAG, "the speaker stopped accepting data");
            break;
        }
        i += (size_t)size + 1;
    }
}

esp_err_t app_audio_play(const uint8_t *amr, size_t len)
{
    app_audio_init();

    if (amr == NULL || len <= AMR_HEADER_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ui_call_busy()) {
        ESP_LOGW(TAG, "not playing: a call has the audio path");
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim()) {
        return ESP_ERR_INVALID_STATE;
    }

    void     *dec     = NULL;
    int16_t  *pcm     = NULL;
    bool      open_ok = false;
    esp_err_t err     = ESP_FAIL;

    s_playing   = true;
    s_stop_play = false;

    if (open_speaker() != ESP_OK) {
        goto done;
    }
    open_ok = true;

    if (esp_amrnb_dec_open(NULL, 0, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "the AMR-NB decoder would not open");
        goto done;
    }

    pcm = wfc_malloc(PCM_BYTES);
    if (pcm == NULL) {
        goto done;
    }

    decode_frames(dec, pcm, amr, len, &s_stop_play);
    err = ESP_OK;

done:
    if (dec != NULL) {
        esp_amrnb_dec_close(dec);
    }
    if (open_ok) {
        esp_codec_dev_close(s_spk);
    }
    wfc_free(pcm);
    s_playing = false;
    release();
    return err;
}

void app_audio_stop_playing(void)
{
    s_stop_play = true;
}

bool app_audio_playing(void)
{
    return s_playing;
}

/* ------------------------------------------------------- playing a stream */

/* Held open between chunks. One task owns all three of these at a time --
 * whoever called open() -- so there is nothing to guard: the latch has
 * already refused everybody else.
 *
 * s_playing is deliberately NOT set while a stream runs. It means "a voice
 * message is playing" to the bubble that draws a stop symbol and to the tap
 * that stops it, and a talk coming out of the speaker is neither of those.
 * What stops something else starting is the latch, which is the honest
 * mechanism and the one that also covers calls. */
static struct {
    void    *dec;
    int16_t *pcm;
    bool     open;
} s_stream;

esp_err_t app_audio_play_open(void)
{
    app_audio_init();

    if (s_stream.open) {
        return ESP_OK;
    }
    if (ui_call_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (open_speaker() != ESP_OK) {
        release();
        return ESP_FAIL;
    }
    if (esp_amrnb_dec_open(NULL, 0, &s_stream.dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "the AMR-NB decoder would not open");
        esp_codec_dev_close(s_spk);
        release();
        return ESP_FAIL;
    }

    s_stream.pcm = wfc_malloc(PCM_BYTES);
    if (s_stream.pcm == NULL) {
        esp_amrnb_dec_close(s_stream.dec);
        s_stream.dec = NULL;
        esp_codec_dev_close(s_spk);
        release();
        return ESP_ERR_NO_MEM;
    }

    s_stream.open = true;
    return ESP_OK;
}

esp_err_t app_audio_play_write(const uint8_t *amr, size_t len)
{
    if (!s_stream.open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (amr == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    decode_frames(s_stream.dec, s_stream.pcm, amr, len, NULL);
    return ESP_OK;
}

void app_audio_play_close(void)
{
    if (!s_stream.open) {
        return;
    }
    s_stream.open = false;

    esp_amrnb_dec_close(s_stream.dec);
    s_stream.dec = NULL;
    wfc_free(s_stream.pcm);
    s_stream.pcm = NULL;
    esp_codec_dev_close(s_spk);
    release();
}
