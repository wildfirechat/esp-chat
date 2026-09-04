/* The audio the application lends this module.
 *
 * wfptt does not open a codec. That is the one structural difference between
 * it and ../wfav-esp, which has a board file of its own, and it is not
 * laziness -- it is the only arrangement that can be correct on this board.
 *
 * There is one I2S bus and two codecs on it, and three features want them:
 * a call (wfav_audio.c), a voice message (the application's app_audio.c) and
 * push-to-talk. Whoever is second has to be told no. A latch can only do that
 * if all three go through it, and no module can hold a latch over the other
 * two -- so the latch belongs to the application, and the modules ask.
 *
 * So this is what the application hands over at wfptt_start(): a microphone
 * that produces AMR-NB and a speaker that consumes it, already arbitrated
 * against everything else that wants them. wfptt supplies the protocol, the
 * channel and the timing.
 *
 * The format is not negotiable in either direction: AMR-NB, 8 kHz, mono, one
 * 20 ms frame per header byte. It is what every other WildFire client
 * records and plays for push-to-talk, so a board that produced anything else
 * would be talking to nobody.
 *
 * ------------------------------------------------------------------------
 * Which task each of these is called on, because two of them may block and
 * the rest may not.
 *
 *   record_*    the wfptt task. record_start() and record_stop() are allowed
 *               to take a moment -- stop waits for a microphone to be put
 *               down -- but nothing here is allowed to wait on the network.
 *   play_open   the wfptt player task, and it MAY BLOCK: it is opening a
 *   play_write  codec and then writing at the speed of real time, which is
 *   play_close  the whole point of it having a task of its own.
 *
 * Neither task holds the display lock and neither is the long link's, so an
 * implementation may do what it likes with the hardware. It may NOT call
 * back into wfptt.
 */

#ifndef WFPTT_AUDIO_H
#define WFPTT_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Opens the microphone and starts encoding. Returns at once; a failure to
     * open may also surface as a recording that never produces any bytes,
     * which this module treats the same way.
     *
     * ESP_ERR_INVALID_STATE is the expected refusal: a call or a voice
     * message has the audio path. It reaches the caller as
     * WFPTT_ERR_RECORDER_ERROR. */
    esp_err_t (*record_start)(void *ud);

    /* The AMR frames recorded so far, WITHOUT the file header, and how many
     * bytes of them there are. The pointer stays valid until record_stop() or
     * record_cancel(), and the length only grows -- this module remembers how
     * far it has sent and publishes the difference.
     *
     * The header is excluded because a sound-data message carries frames and
     * nothing else: "#!AMR\n" in the middle of a stream is six bytes the far
     * end will try to decode as a frame. The same recording keeps its header
     * when it comes back whole from record_stop(), because there it is a
     * file. */
    const uint8_t *(*record_data)(void *ud, size_t *len);

    /* Stops, and hands over the whole take as a playable .amr -- header and
     * all -- for the voice message that optionally follows a talk. On ESP_OK
     * this module owns `amr` and passes it to the on_recording callback (or
     * frees it).
     *
     * Any other return means there is no take, which is not an error: a press
     * too short to be a message is the usual reason (ESP_ERR_INVALID_SIZE).
     * The talk still ends normally -- the audio was already sent, chunk by
     * chunk, while it was happening. */
    esp_err_t (*record_stop)(void *ud, uint8_t **amr, size_t *len, int *seconds);

    /* Stops and throws it away. Called when a talk is abandoned rather than
     * finished -- wfptt_stop() with the microphone open. */
    void (*record_cancel)(void *ud);

    /* Opens the speaker for a stream of frames. Called when the first chunk
     * of somebody's talk arrives, not when the module starts, so a board
     * nobody talks to never touches the codec. */
    esp_err_t (*play_open)(void *ud);

    /* Decodes and plays one chunk, blocking for as long as the audio lasts.
     * That blocking is the pacing: the queue in front of it is the jitter
     * buffer, and it is allowed to fill. */
    esp_err_t (*play_write)(void *ud, const uint8_t *amr, size_t len);

    /* Closes the speaker and gives the audio path back. Called when a talker
     * stops, and when this board takes the channel to talk itself. */
    void (*play_close)(void *ud);

    /* Handed to every function above. */
    void *ud;
} wfptt_audio_t;

#ifdef __cplusplus
}
#endif

#endif /* WFPTT_AUDIO_H */
