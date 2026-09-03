/* One call, from the outside.
 *
 * callSession.js: what a call screen reads to draw itself and what its
 * buttons call. Everything here is safe from any task -- the reads take a
 * short mutex and copy out, the writes post to the AV task and return -- so
 * an LVGL event callback holding the display lock may use any of it.
 *
 * ------------------------------------------------------------------------
 * There is no session handle, on purpose.
 *
 * This board has one microphone and one speaker, so it is in exactly one call
 * or in none. An API that took a `wfav_session_t *` and then ignored it --
 * which is what a handle would be here -- would describe a system that does
 * not exist, and it would oblige every caller to hold a pointer across
 * redraws that the end of a call could invalidate underneath them.
 *
 * So the call is implicit and every getter answers for whichever one is up.
 * With no call, each returns the empty version of itself: IDLE, "", 0, no
 * participants. A screen that redraws one frame after the call ended draws an
 * idle call screen rather than crashing, and closes itself on the next.
 */

#ifndef WFAV_SESSION_H
#define WFAV_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfav_types.h"
#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- reading */

wfav_call_state_t wfav_session_state(void);

/* The call's ID, the conversation it belongs to, and who started it. The ID
 * is what every signalling message is keyed on, so a UI that logs one line
 * per call logs this. Never NULL; "" when there is no call. */
const char *wfav_session_call_id(void);
const char *wfav_session_initiator(void);
void        wfav_session_conversation(wfc_conversation_t *out);

/* True when this board placed the call. */
bool wfav_session_is_outgoing(void);

/* True while audio is muted at the microphone. */
bool wfav_session_audio_muted(void);

/* How long media has been flowing, in milliseconds, or 0 before it started.
 * This is what a call screen's running timer shows, and it is measured from
 * the moment the first PeerConnection connected rather than from the answer:
 * a call that takes four seconds to pair should not claim four seconds of
 * talking. */
int64_t wfav_session_duration_ms(void);

/* The other people in the call, copied into `out`. Returns how many were
 * written, at most `max`.
 *
 * For a 1v1 call this is one entry and the answer to "who am I talking to".
 * It is a copy because the alternative is holding the session's lock across
 * a redraw, and a redraw is exactly when the AV task wants the lock to
 * process a participant leaving. */
size_t wfav_session_participants(wfav_participant_t *out, size_t max);

/* The one other person, for the 1v1 case that is all this build makes. Writes
 * "" and returns false when the call has no participants yet. */
bool wfav_session_peer(char *buf, size_t buf_size);

/* ------------------------------------------------------------ commands */

/* Answers an incoming call. A no-op unless the state is INCOMING, which makes
 * a double tap on the answer button harmless.
 *
 * Returns once the request is queued. The accept message goes out, the state
 * becomes CONNECTING, and the room work happens on the AV task. */
esp_err_t wfav_answer(void);

/* Ends the call: rejects it while ringing, hangs up once connected. Both are
 * the same message with a different reason, and the reason is chosen from the
 * state, so a UI has one button for both. */
esp_err_t wfav_hangup(void);

/* Mutes or unmutes the microphone. Takes effect locally at once and is
 * reported to the room, so the other end's UI can show it. */
esp_err_t wfav_set_audio_muted(bool muted);

#ifdef __cplusplus
}
#endif

#endif /* WFAV_SESSION_H */
