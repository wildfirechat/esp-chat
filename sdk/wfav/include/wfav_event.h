/* Typed call events, on the same terms wfc_event.h sets.
 *
 * This is callSessionCallback.js, minus the video half and plus C's need to
 * name each callback's type. One typedef and one subscribe function per
 * event, with the callback held in a union rather than behind a cast, for the
 * reason wfc_event.c spells out: an event that grows a parameter should
 * produce a list of compile errors, not a board reading a struct that is no
 * longer there.
 *
 * ------------------------------------------------------------------------
 * Threading, and the part that differs from wfc_event.h.
 *
 * Every callback runs on the AV task -- wfav's own, not the long link's.
 * That is deliberate: the call state machine is driven from three different
 * places (an IM message arriving on the wfc_mqtt task, a Janus reply arriving
 * on the same one, a PeerConnection state change arriving on esp_peer's), and
 * funnelling all three onto one task is what makes the state machine
 * single-threaded and therefore readable.
 *
 * The rules that follow are wfc_event.h's:
 *
 *   - do not block in a callback. The AV task is also what answers Janus's
 *     keepalive, and a call whose keepalive stops is a call the room drops.
 *   - do not touch LVGL. Post to the UI (ui.h) or use lv_async_call.
 *   - do not call wfav_engine_stop() or wfav_hangup() and then wait for
 *     anything; hangup itself is safe and is the normal thing to do.
 *
 * Subscribing and unsubscribing are safe from any task and safe from inside a
 * callback, including cancelling the subscription that is running.
 *
 * Lifetimes: every pointer handed to a callback dies with the call. A user ID
 * worth keeping gets copied.
 */

#ifndef WFAV_EVENT_H
#define WFAV_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wfav_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wfav_subscription wfav_subscription_t;

/* --------------------------------------------------------------- events */

/* A call appeared. `incoming` is false when this board started it.
 *
 * Raised once per call, before any state change, and it is the cue to put a
 * call screen up. Everything else about the call is read back through
 * wfav_session.h rather than passed here, so that a screen redrawing for any
 * other reason reads the same values this event would have carried. */
typedef void (*wfav_on_call_started_t)(const char *call_id, bool incoming,
                                       void *ud);

/* The state moved. See wfav_call_state_t.
 *
 * WFAV_STATE_IDLE is NOT raised here -- a call ending is its own event below,
 * because a UI that closes on IDLE would close before it could say why. */
typedef void (*wfav_on_state_changed_t)(wfav_call_state_t state, void *ud);

/* The call is over, for this reason, and the session is gone by the time this
 * returns. `duration_ms` is how long media was actually flowing -- 0 for a
 * call that never connected, which is what makes a missed call distinguish
 * itself from a short one. */
typedef void (*wfav_on_call_ended_t)(wfav_end_reason_t reason,
                                     int64_t duration_ms, void *ud);

/* Someone joined the room, their media connected, or they left. `reason` on
 * the last one is why they in particular went, which is not necessarily why
 * the call ends. */
typedef void (*wfav_on_participant_joined_t)(const char *user_id, void *ud);
typedef void (*wfav_on_participant_connected_t)(const char *user_id, void *ud);
typedef void (*wfav_on_participant_left_t)(const char *user_id,
                                           wfav_end_reason_t reason, void *ud);

/* Someone's microphone was muted or unmuted -- ours included, which is how a
 * UI learns that its own mute button took effect rather than assuming it. */
typedef void (*wfav_on_mute_changed_t)(const char *user_id, bool audio_muted,
                                       void *ud);

/* Where an error's `code` comes from.
 *
 * Three numbering spaces used to arrive through this callback as one `int`:
 * end reasons, janus.plugin.videoroom's error codes, and the transport's
 * reply codes. A UI given 4 could not tell "media error" from "room error 4",
 * so it could only ever print `detail`. The domain makes the number mean
 * something again. */
typedef enum {
    WFAV_ERR_MEDIA = 0,  /* code is a wfav_end_reason_t */
    WFAV_ERR_ROOM,       /* code is videoroom's, or the transport's reply */
} wfav_error_domain_t;

/* Something went wrong that did not end the call: a Janus request refused, a
 * subscriber that would not come up. `detail` is a short English string for
 * the log and dies with the call. */
typedef void (*wfav_on_error_t)(wfav_error_domain_t domain, int code,
                                const char *detail, void *ud);

/* ---------------------------------------------------------- subscribing */

wfav_subscription_t *wfav_on_call_started(wfav_on_call_started_t cb, void *ud);
wfav_subscription_t *wfav_on_state_changed(wfav_on_state_changed_t cb, void *ud);
wfav_subscription_t *wfav_on_call_ended(wfav_on_call_ended_t cb, void *ud);
wfav_subscription_t *wfav_on_participant_joined(wfav_on_participant_joined_t cb,
                                                void *ud);
wfav_subscription_t *wfav_on_participant_connected(
    wfav_on_participant_connected_t cb, void *ud);
wfav_subscription_t *wfav_on_participant_left(wfav_on_participant_left_t cb,
                                              void *ud);
wfav_subscription_t *wfav_on_mute_changed(wfav_on_mute_changed_t cb, void *ud);
wfav_subscription_t *wfav_on_error(wfav_on_error_t cb, void *ud);

/* Safe on NULL, and safe from inside the callback being cancelled. The handle
 * is dead afterwards. */
void wfav_unsubscribe(wfav_subscription_t *sub);

/* Cancels each and NULLs the entries -- one line to tear down a screen's
 * subscriptions. */
void wfav_unsubscribe_all(wfav_subscription_t **subs, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* WFAV_EVENT_H */
