/* Push-to-talk: hold a button, everyone in the conversation hears you.
 *
 * This is ptt.js and android-pttclient, in C and on one board. Like them it
 * is a module ON TOP of the IM client rather than a second protocol: a talk
 * is a burst of ordinary WFC messages on the same long link, and there is
 * nothing here the IM client does not already expose.
 *
 *   out   wfc_send_message()   pttStart, then a pttSoundData every 400 ms,
 *                              then pttEnd
 *   out   wfc_require_lock()   who gets the microphone, where only one may
 *         wfc_release_lock()   have it
 *   in    wfc_on_receive_messages()   the same three, from everyone else
 *   both  wfc_get_user_setting()      whether this channel is muted here
 *
 * That is the whole seam, and it is the reason this module can be deleted
 * from a build without the IM client noticing (see the README).
 *
 * ------------------------------------------------------------------------
 * What a talk actually is on the wire.
 *
 * The audio is AMR-NB at 12.2 kbps -- 32 bytes per 20 ms frame -- carried raw
 * in MessageContent.data with no file header, twenty frames to a message.
 * One message every 400 ms, which is ptt.js's cadence
 * (pttRecordSession.js's setInterval) rather than android-pttclient's 100 ms,
 * and the difference is this board's link: every message is a PUBLISH waiting
 * for a PUBACK in one of sixteen in-flight slots (wfc_mqtt.c), and ten of
 * them a second leaves no room for anything else the client has to say. The
 * cost is 400 ms of mouth-to-ear delay on top of the network, which is what
 * the reference web client already lives with.
 *
 * Nothing is stored: the three signalling types are transparent, so a talk
 * leaves no messages behind and adds nothing to anyone's unread badge. What
 * it optionally leaves behind is one voice message per press -- see
 * `on_recording` below.
 *
 * ------------------------------------------------------------------------
 * One speaker, and what that means for listening.
 *
 * The reference clients play everybody at once: a session per talker, mixed
 * by the platform's audio stack. This board has one I2S bus, one codec and no
 * mixer, so it plays ONE talker and reports the rest.
 *
 * Which one: the first to be heard from keeps the speaker until they stop,
 * unless someone with a HIGHER priority starts -- which is exactly the
 * "priority mode" ptt.js documents as its alternative to mixing, and it is
 * the mode the hardware forces here. Everyone talking is still tracked and
 * still reported through wfptt_on_user_start_talking(), so a screen can show
 * three names and be honest that it is playing one of them.
 *
 * Talking is half duplex for the same reason: taking the channel closes the
 * speaker, because the microphone and the speaker are the same bus and the
 * board cannot have both open at two different times. ENABLE_FULL_DUPLEX is
 * false in both reference clients too.
 *
 * ------------------------------------------------------------------------
 * Who may talk, and the lock.
 *
 * A channel that allows one speaker is arbitrated by the server: SLT, WFC's
 * distributed lock (wfc_client.h), named "WFPTT_" plus the two user IDs in
 * sorted order for a single chat and plus type+target+line for a group. The
 * sort matters -- it is what makes both ends name the SAME lock -- and it is
 * android-pttclient's formula rather than ptt.js's, which uses the
 * conversation target and therefore has each side locking a different name.
 *
 * A channel that allows several does not lock at all; it counts the talkers
 * it can see and refuses past the limit, which is what the reference clients
 * do and is approximate by nature -- two boards pressing at the same instant
 * both see a channel with room in it.
 *
 * The lock expires by itself after WFPTT_LOCK_SECONDS, and this module does
 * NOT renew it while a talk runs. That is deliberate and it is the reference
 * behaviour: the expiry is the safety net for a client that crashes holding
 * the channel, and a talk that outlives it can be interrupted -- which on a
 * walkie-talkie is the right way round, since the alternative is a channel
 * nobody can take back for a minute.
 *
 * ------------------------------------------------------------------------
 * Listening is global, muting is per conversation.
 *
 * Once started, this module plays push-to-talk from ANY conversation, which
 * is ENABLE_GLOBAL_PTT in both references and is the thing that makes a board
 * on a shelf a walkie-talkie rather than a screen you have to be looking at.
 *
 * The off switch is per conversation and is an account setting rather than a
 * board one -- wfptt_set_silent() writes user setting scope 25, the same one
 * the phone writes -- so a channel muted on the phone is muted here.
 *
 * ------------------------------------------------------------------------
 * Threading.
 *
 * Everything below is safe from any task, including from an LVGL button
 * callback: the two that do anything post to the wfptt task and return.
 * wfptt_status() and wfptt_talkers() read state that is only written on that
 * task, so they are cheap enough for a repaint.
 *
 * The one that is not safe from a button is wfptt_set_silent(), which reaches
 * a blocking send() on the long link -- the same rule wfc_set_user_setting()
 * carries.
 */

#ifndef WFPTT_CLIENT_H
#define WFPTT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_client.h"
#include "wfc_model.h"

#include "wfptt_audio.h"
#include "wfptt_event.h"
#include "wfptt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one press of the button produced, handed over when the talk ends.
 *
 * This is the sendVoiceMessage half of the reference clients: the burst that
 * was streamed chunk by chunk is ALSO a complete .amr, and uploading it as an
 * ordinary voice message is what leaves a record of the conversation for
 * anyone who was not listening at the time.
 *
 * It is a callback rather than something this module does because the upload
 * is a GMPU round trip and then an HTTP PUT of up to 96 KB, which is seconds
 * -- and the application already has a task for exactly that (a voice message
 * recorded by hand takes the same path). Doing it here would mean a second
 * uploader, and a talk that could not start again until the last one had
 * finished going up.
 *
 * OWNERSHIP OF `amr` PASSES TO THE CALLBACK, which frees it with wfc_free()
 * whether it managed to send it or not. It is up to 96 KB of PSRAM and the
 * one thing that must not happen to it is being leaked on the failure path.
 *
 * `conv` is the caller's and dies with the call; copy it. Runs on the wfptt
 * task, so it must not block -- park the buffer and return.
 *
 * Leave it NULL and the recording is dropped, which is the right setting for
 * a channel that is meant to be ephemeral. */
typedef void (*wfptt_on_recording_t)(const wfc_conversation_t *conv,
                                     uint8_t *amr, size_t len, int seconds,
                                     void *ud);

typedef struct {
    /* The microphone and the speaker, already arbitrated against the other
     * things that want them (wfptt_audio.h). Required. */
    wfptt_audio_t audio;

    /* What to do with the whole take. May be NULL. */
    wfptt_on_recording_t on_recording;
    void                *ud;

    /* This board's talking priority, 0 unless a deployment has a reason.
     * Higher wins: a listener drops whoever it is playing for a talker with a
     * bigger number. It rides on every pttStart and every pttSoundData, so
     * the far end can act on it without having caught the start. */
    int32_t priority;
} wfptt_config_t;

/* Registers the four content types and subscribes to the message stream.
 *
 * Call it after wfc_client_init() and BEFORE wfc_client_connect(), like every
 * other content-type registration: a conversation row's digest is computed as
 * the message is filed, and a type registered late does not go back and fix
 * the rows that were filed without it (wfc_content.h). It is also the
 * ordinary reason to be listening early -- the first thing a board that was
 * just switched on is likely to get is somebody already talking.
 *
 * Cheap in the way that matters: two small tasks and a queue, and NO codec is
 * touched until there is actually audio -- a board nobody talks to never opens
 * the speaker. Calling it twice is a no-op with a warning. */
esp_err_t wfptt_start(const wfptt_config_t *cfg);

/* Stops everything: any talk in progress is abandoned, the speaker is
 * closed, the subscription is dropped. Blocks until the tasks are gone.
 * Never from a callback. */
void wfptt_stop(void);

bool wfptt_running(void);

/* --------------------------------------------------------------- talking */

/* Ask for the channel. Returns as soon as the request is posted -- whether it
 * was granted arrives as wfptt_on_talk_begin() or wfptt_on_talk_failed(),
 * because on a one-speaker channel the answer is a round trip to the server.
 *
 * Safe from an LVGL button's LV_EVENT_PRESSED.
 *
 * Asking again while talking or while waiting fails with
 * WFPTT_ERR_TALKING. */
esp_err_t wfptt_request_talk(const wfc_conversation_t *conv);

/* Let it go. The pair of the call above, and the one that has to happen: a
 * button whose release is missed is a board that talks until the cap.
 *
 * Safe to call when not talking, and safe while still waiting for the lock --
 * in which case the channel is given back as soon as it arrives, and no
 * audio is ever sent. Safe from LV_EVENT_RELEASED. */
void wfptt_release_talk(void);

/* --------------------------------------------------------------- reading */

typedef struct {
    wfptt_state_t      state;
    /* What this board is talking on. Meaningless when IDLE. */
    wfc_conversation_t conv;
    /* How long the current talk has run, and the cap it is running against.
     * Both in seconds, so a screen can draw a countdown without a second
     * source of truth. */
    int                talk_seconds;
    int                max_seconds;

    /* Who is being PLAYED right now, "" for nobody -- one talker, for the
     * reason at the top of this file. `speaker_conv` is which conversation
     * they are talking in, which is not necessarily the one this board is
     * looking at: listening is global. */
    char               speaker[WFC_TARGET_MAX];
    wfc_conversation_t speaker_conv;

    /* How many people this module currently believes are talking, everywhere.
     * wfptt_talkers() is the same answer for one conversation. */
    size_t             talkers;

    /* The last refusal and the last ending, kept so a screen that repaints
     * after the event has gone by can still say what happened. 0 / -1 when
     * nothing has. */
    int                last_error;
    int                last_end_reason;
} wfptt_status_t;

void wfptt_status(wfptt_status_t *out);

/* Who is talking in one conversation, into a caller-supplied array. Returns
 * how many were written, which is at most `max` and at most
 * WFPTT_MAX_TALKERS. */
size_t wfptt_talkers(const wfc_conversation_t *conv,
                     char uids[][WFC_TARGET_MAX], size_t max);

/* ---------------------------------------------------------------- muting */

/* Whether this conversation's push-to-talk is silenced for this ACCOUNT --
 * user setting scope 25, keyed "<target>-<type>-<line>".
 *
 * Note the key's shape: target first. It is not the order the two
 * conversation settings wfc-esp already writes use (type-line-target), and
 * that is not a mistake in either place -- push-to-talk's key was written
 * this way in the reference clients and both ends have to agree with the
 * phone, not with each other.
 *
 * The read answers from the store and never fetches. The write does not wait:
 * ESP_OK means "sent", the local row appears when the server acknowledges it,
 * and a refused change simply does not happen. NOT safe from an LVGL
 * callback. */
bool      wfptt_is_silent(const wfc_conversation_t *conv);
esp_err_t wfptt_set_silent(const wfc_conversation_t *conv, bool silent);

#ifdef __cplusplus
}
#endif

#endif /* WFPTT_CLIENT_H */
