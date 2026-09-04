/* Typed push-to-talk events, on the terms wfc_event.h and wfav_event.h set.
 *
 * Five events, and they are the two callbacks the reference clients have
 * flattened out: TalkingCallback is what happens to OUR press of the button
 * (begin / end / refused), PttCallback is what happens to somebody ELSE's
 * (started / stopped). One typedef and one subscribe function each, with the
 * callback held in a union rather than behind a cast -- wfc_event.c explains
 * why at length, and the short version is that an event which grows a
 * parameter should produce a list of compile errors.
 *
 * ------------------------------------------------------------------------
 * Threading.
 *
 * Every callback runs on the wfptt task. Not the long link's, and not the
 * player's: an IM message arriving, a lock reply arriving and the talk timer
 * expiring are three different tasks' worth of input, and funnelling them
 * onto one is what makes the state machine single threaded.
 *
 * The usual three rules follow:
 *
 *   - do not block. This task is also what publishes a chunk of audio every
 *     400 ms, and a chunk that is late is a gap the far end hears.
 *   - do not touch LVGL. Post to the UI.
 *   - do not call wfptt_stop() and then wait for anything. wfptt_release_talk()
 *     is safe and is the normal thing to do.
 *
 * Subscribing and unsubscribing are safe from any task, including from inside
 * the callback being cancelled.
 *
 * Every pointer handed to a callback dies with the call.
 */

#ifndef WFPTT_EVENT_H
#define WFPTT_EVENT_H

#include <stdbool.h>
#include <stddef.h>

#include "wfc_model.h"
#include "wfptt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wfptt_subscription wfptt_subscription_t;

/* --------------------------------------------------------------- events */

/* The channel is ours: the microphone is open and the first chunk is on its
 * way. This is the cue to show a talking indicator, and on hardware with one
 * it is the cue to beep.
 *
 * On a channel that allows one speaker this is a round trip after the button
 * went down -- the lock had to be asked for -- so it is an event rather than
 * a return value. */
typedef void (*wfptt_on_talk_begin_t)(const wfc_conversation_t *conv, void *ud);

/* Our talk is over, for this reason: the button came up, the cap was
 * reached, the group muted us, the link went away. The microphone is already
 * closed by the time this runs. */
typedef void (*wfptt_on_talk_end_t)(const wfc_conversation_t *conv,
                                    wfptt_end_reason_t reason, void *ud);

/* The channel was refused. `error_code` is one of the WFPTT_ERR_* values, and
 * the one worth telling a person about by name is OCCUPIED: somebody else is
 * holding the microphone. Nothing was opened and nothing was sent. */
typedef void (*wfptt_on_talk_failed_t)(const wfc_conversation_t *conv,
                                       int error_code, void *ud);

/* Somebody started talking, or stopped.
 *
 * "Stopped" is either their pttEnd or their silence: a talker whose audio has
 * not arrived for WFPTT_TALKER_TIMEOUT_MS is treated as gone, because a
 * client that was switched off mid-sentence sends no end notification and the
 * channel would otherwise stay occupied forever.
 *
 * Being told somebody is talking is not the same as hearing them. This board
 * has one speaker and no mixer, so it plays one talker at a time
 * (wfptt_client.h); the others are still reported here, which is what lets a
 * screen show a channel with three people on it. */
typedef void (*wfptt_on_user_start_talking_t)(const wfc_conversation_t *conv,
                                              const char *user_id, void *ud);
typedef void (*wfptt_on_user_end_talking_t)(const wfc_conversation_t *conv,
                                            const char *user_id, void *ud);

/* ---------------------------------------------------------- subscribing */

wfptt_subscription_t *wfptt_on_talk_begin(wfptt_on_talk_begin_t cb, void *ud);
wfptt_subscription_t *wfptt_on_talk_end(wfptt_on_talk_end_t cb, void *ud);
wfptt_subscription_t *wfptt_on_talk_failed(wfptt_on_talk_failed_t cb, void *ud);
wfptt_subscription_t *wfptt_on_user_start_talking(wfptt_on_user_start_talking_t cb,
                                                  void *ud);
wfptt_subscription_t *wfptt_on_user_end_talking(wfptt_on_user_end_talking_t cb,
                                                void *ud);

/* Safe on NULL, and safe from inside the callback being cancelled. */
void wfptt_unsubscribe(wfptt_subscription_t *sub);

/* Cancels each and NULLs the entries -- one line to tear a screen down. */
void wfptt_unsubscribe_all(wfptt_subscription_t **subs, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* WFPTT_EVENT_H */
