/* Voice messages: where the bytes come from and where they go.
 *
 * The same split pictures have. ui_msg_voice.c is the view -- a bubble, a
 * duration, a tap -- and everything that takes time is here, on tasks of this
 * file's own. A view asks a question that is always cheap to answer and a
 * page posts a job that returns at once; neither ever waits.
 *
 * ------------------------------------------------------------------------
 * Why this cannot live in a page's prime(), which is where sending normally
 * goes.
 *
 * prime() may block, and the chat page uses that to send text: wfc_send_text()
 * ends in one write on an open socket, which is milliseconds. A voice message
 * is not that. It is a GMUT round trip and then an HTTP upload of up to 96 KB,
 * which on a slow link is ten seconds -- and prime() runs on the UI task, so
 * ten seconds there is ten seconds in which the back arrow does nothing. That
 * is the failure ui_media.h describes for pictures, in the other direction.
 *
 * ------------------------------------------------------------------------
 * Two tasks, not one.
 *
 * Playing and sending are both slow and they are NOT the same kind of slow:
 * sending is the network, playing is the codec and the speaker. One task
 * would make a tap-to-play wait behind an upload that has nothing to do with
 * it. They do contend for the audio path itself, but that is app_audio.c's
 * latch to hold and it holds it for the length of the audio, not the length
 * of the upload.
 */

#ifndef UI_VOICE_H
#define UI_VOICE_H

#include <stddef.h>
#include <stdint.h>

#include "wfc_model.h"

/* Creates the two tasks. Once, from ui_init().
 *
 * Up front rather than on first use, which is where this differs from
 * ui_media_start(): a picture's fetcher can be created when the first picture
 * is looked at because nothing is waiting on it, while a tap that has to wait
 * for a task to be created is a tap that looks like it did nothing. */
void ui_voice_start(void);

/* Plays the voice message with this UID: the URL out of the store, the file
 * over HTTP, the audio out of the speaker. Returns at once.
 *
 * Asking for one while one is playing does nothing -- see ui_msg_voice.c for
 * why that is the honest answer rather than a queue. */
void ui_voice_play(int64_t message_uid);

/* True while a message is being played, and which one. The view draws from
 * these, so both are cheap and neither blocks. */
bool    ui_voice_playing(void);
int64_t ui_voice_playing_uid(void);

/* Stops whatever is playing. */
void ui_voice_stop(void);

/* Uploads `amr` and sends it to `conv` as a voice message. Returns at once.
 *
 * TAKES OWNERSHIP of `amr` and frees it when it is done with it, whether the
 * send worked or not -- the recorder's buffer is 96 KB of PSRAM and the one
 * thing that must not happen to it is being leaked on the failure path.
 *
 * `conv` is copied. The page that asked may be long gone by the time this
 * finishes, which is the point: a voice message keeps uploading while you
 * navigate away. */
void ui_voice_send(const wfc_conversation_t *conv, uint8_t *amr, size_t len,
                   int seconds);

/* The same thing, sent as the push-to-talk flavour of a voice message
 * (content type 23 rather than 2): what one press of the 对讲 button leaves
 * behind, so a channel keeps a record for whoever was not listening at the
 * time.
 *
 * It goes on the SAME queue and through the same task, which is the whole
 * reason it is here rather than in ../wfptt-esp: there is one uploader on
 * this board, and a talk that had its own would mean a second 96 KB in
 * flight and a talk that could not start until the last one had gone up.
 *
 * Identical contract to the above -- takes ownership of `amr`, copies `conv`,
 * returns at once. The two differ in one number on the wire and one word in
 * the conversation list. */
void ui_voice_send_ptt(const wfc_conversation_t *conv, uint8_t *amr, size_t len,
                       int seconds);

#endif /* UI_VOICE_H */
