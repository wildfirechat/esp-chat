/* Recording and playing one voice message.
 *
 * The format is AMR-NB, and that is not a preference -- it is what every
 * other WildFire client reads. The web client records .amr through
 * benz-amr-recorder, proto2 names the extension in getMediaPath()
 * (business.cc:2200), and the phones use the platform AMR codec. A voice
 * message in any other container is a message this board can send and nobody
 * can play, which is worse than not sending one.
 *
 * ESP-IDF's esp_audio_codec has both halves of AMR-NB in its prebuilt
 * library, so this file is a microphone, an encoder, a speaker and a decoder
 * wired together -- no codec to port and no third-party source to vendor.
 *
 * ------------------------------------------------------------------------
 * Recording and playing are mutually exclusive, and so are voice messages and
 * calls.
 *
 * The board has one I2S bus, and its two codecs share it: the ES8311 on the
 * way out and the ES7210 on the way in, opened at one sample rate between
 * them. A call owns both for as long as it lasts (wfav_audio.c), so a record
 * or a play that started during one would be reconfiguring the bus underneath
 * it. Both entry points below refuse while ui_call_busy(), and one latch
 * keeps recording and playback off each other.
 *
 * ------------------------------------------------------------------------
 * Neither entry point may be called from a place that must not block.
 *
 * app_audio_play() decodes and writes for as long as the message lasts, which
 * is seconds. app_audio_record_start() returns at once -- the recording runs
 * on a task of this file's own -- but record_stop() waits for that task to
 * put the microphone down. So: a page's prime(), or a task of the caller's,
 * never create()/refresh()/draw() and never a wfc callback.
 *
 * app_audio_record_cancel() is the exception and is written to be one, because
 * the page that has to call it calls it from destroy().
 */

#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The longest message this board will record.
 *
 * At AMR-NB's top bitrate a second is 1600 bytes, so this is what decides the
 * buffer -- 96 KB, allocated in PSRAM when recording starts and freed when it
 * ends. It is also roughly the point past which a voice message stops being
 * a voice message, which is why nobody's is longer. */
#define APP_AUDIO_MAX_SECONDS 60

/* Creates the one-at-a-time latch. Once, from ui_init(). Cheap -- neither
 * codec is touched until something is actually recorded or played. */
void app_audio_init(void);

/* Starts recording. Returns at once; the microphone is opened on the
 * recorder task, so a failure to open it shows up as a recording that stops
 * itself with nothing in it rather than as an error here.
 *
 * ESP_ERR_INVALID_STATE means a call is up, or something is already using the
 * audio path. */
esp_err_t app_audio_record_start(void);

/* The recording SO FAR, without the "#!AMR\n" header, and how many bytes of
 * it there are. NULL and 0 until there is a frame.
 *
 * This is the whole of what streaming adds, and it adds nothing to the
 * recorder: the buffer is allocated whole when a recording starts and only
 * ever grows, so a caller that remembers how far it has read can take the
 * difference whenever it likes. Push-to-talk does that every 400 ms
 * (wfptt_audio.h); a voice message never asks and gets the same recording
 * complete at the end.
 *
 * The pointer is valid until record_stop() or record_cancel(). The length is
 * read afresh each call and may have grown by the time the caller uses it,
 * which is safe in the one direction that matters -- what is already there
 * does not move.
 *
 * The header is left out because a chunk of a stream is frames and nothing
 * else: six bytes of magic in the middle of one is something the far end
 * would try to decode. The same recording keeps its header when it comes back
 * whole from record_stop(), because there it is a file. */
const uint8_t *app_audio_record_body(size_t *len);

/* For the page to draw. Safe from anywhere, including draw(). */
bool app_audio_recording(void);
int  app_audio_record_seconds(void);

/* Stops, and hands over what was recorded. On ESP_OK the caller owns `amr`
 * and frees it with wfc_free(); `seconds` is what goes in the message's
 * duration field, rounded up so a 1.4-second message does not display as 1.
 *
 * ESP_ERR_INVALID_SIZE means the recording was too short to send -- under a
 * second, which on this board is usually a mis-tap rather than a message. */
esp_err_t app_audio_record_stop(uint8_t **amr, size_t *len, int *seconds);

/* Stops and throws the recording away. Safe to call when not recording, and
 * unlike record_stop() it DOES NOT BLOCK: the recorder task notices within a
 * frame and releases the buffer itself. That is what makes it callable from
 * the record page's destroy(), which runs with the display lock held. */
void app_audio_record_cancel(void);

/* Decodes and plays. Blocks until it has finished or been interrupted by
 * app_audio_stop_playing(). */
esp_err_t app_audio_play(const uint8_t *amr, size_t len);

/* The same speaker, opened once and written to many times: what a live
 * stream of somebody else's voice needs and a message does not (push-to-talk,
 * again).
 *
 * open() takes the audio path and holds it until close(), so the three of
 * them belong to one task and that task must not sit on them -- a stream that
 * has gone quiet closes rather than waiting for more.
 *
 * ESP_ERR_INVALID_STATE from open() is the ordinary refusal: a call, a
 * recording, or a voice message has the codec.
 *
 * write() blocks for as long as the audio it is given lasts, which is what
 * makes whatever queue is in front of it a jitter buffer rather than a place
 * frames pile up. A frame that will not decode ends that chunk and not the
 * stream: one bad message should cost a syllable, not the conversation. */
esp_err_t app_audio_play_open(void);
esp_err_t app_audio_play_write(const uint8_t *amr, size_t len);
void      app_audio_play_close(void);

/* Cuts a play short. Safe from any task, including from a button callback
 * while app_audio_play() is running on another. */
void app_audio_stop_playing(void);

bool app_audio_playing(void);

/* How long an AMR-NB file runs, from its frame headers alone -- every frame
 * is 20 ms and its size is fixed by its mode, so this is a walk rather than a
 * decode.
 *
 * It is the fallback for a message whose sender did not fill in a duration,
 * which the reference clients do not always do. */
int app_audio_amr_seconds(const uint8_t *amr, size_t len);

#endif /* APP_AUDIO_H */
