/* Typed event subscription.
 *
 * The problem this solves is the one uni-chat-x's wfcEventSubscriber.uts
 * solves, arrived at from the C side. An event bus wants to be generic, so
 * its natural shape is `(event_id, void *args)` and every subscriber starts
 * with a cast. Change what an event carries and nothing complains until the
 * board is on a desk somewhere reading a struct that is no longer there.
 *
 * So: one named callback typedef and one named subscribe function per event.
 * Underneath, wfc_event.c holds the callback in a union of those twelve types
 * rather than casting a generic pointer, so there is no unchecked step at
 * all. Changing what an event carries produces a list of the callers that
 * have to change with it, which is the entire point.
 *
 * ------------------------------------------------------------------------
 * Threading. Every callback runs on the task that raised the event. For
 * anything the server caused -- messages, profiles, the friend list, the
 * link's state -- that is the wfc_mqtt task, the one reading the socket. An
 * event raised by an API call (wfc_clear_unread() is the one) runs on
 * whichever task made the call. So, for the common case:
 *
 *   - do not block in a callback. The long link stops being read while you
 *     are in there, and the server's keepalive does not care why.
 *   - do not touch LVGL directly. The widget tree belongs to the UI task;
 *     post to it (ui.h does) or use lv_async_call.
 *   - do not call wfc_client_disconnect() from one. It waits for the task
 *     you are standing on.
 *
 * Subscribing and unsubscribing, by contrast, are safe from any task and
 * safe from inside a callback -- including unsubscribing the subscription
 * that is currently running.
 *
 * Lifetimes follow wfc_model.h: a wfc_message_t handed to a callback dies
 * with that call, while profiles and conversation entries are copies the
 * callback may keep.
 */

#ifndef WFC_EVENT_H
#define WFC_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One subscription. Keep it if the subscriber can go away -- an LVGL screen
 * being replaced is the case that matters, since a callback holding a freed
 * lv_obj_t * is a crash that happens somewhere else entirely. A subscriber
 * that lives for the life of the program can throw the handle away. */
typedef struct wfc_subscription wfc_subscription_t;

/* --------------------------------------------------------------- events */

/* The link's state, as wfc_connection_status_t in wfc_client.h. Negative
 * values are failures and the client is not going to retry on its own; 1 is
 * the working state. */
typedef void (*wfc_on_connection_status_t)(int status, void *ud);

/* A batch of messages, oldest first, already stored and deduplicated.
 * `has_more` is true while the catch-up still has rounds to run, which is the
 * cue to hold off an expensive redraw until it settles. */
typedef void (*wfc_on_receive_messages_t)(const wfc_message_t *msgs, size_t n,
                                          bool has_more, void *ud);

/* One of our own messages came back from the server. `error_code` is a
 * wfc_mqtt reply code: 0 means sent, and the uid and timestamp are the
 * server's. */
typedef void (*wfc_on_send_result_t)(int error_code, int64_t message_uid,
                                     int64_t timestamp, void *ud);

typedef void (*wfc_on_recall_message_t)(const char *operator_uid,
                                        int64_t message_uid, void *ud);

/* A conversation's row changed: new last message, new unread count, or it was
 * just marked read. The entry is a copy. */
typedef void (*wfc_on_conversation_update_t)(const wfc_conversation_info_t *info,
                                             void *ud);

/* Profiles that just arrived from the server and are now in the store. The
 * arrays are the caller's scratch and die with the call; the store has them
 * if you want them later. */
typedef void (*wfc_on_user_infos_update_t)(const wfc_user_info_t *users, size_t n,
                                           void *ud);
typedef void (*wfc_on_group_infos_update_t)(const wfc_group_info_t *groups, size_t n,
                                            void *ud);

/* `n` members of `group_id` changed. The roster itself is not passed --
 * a large group would mean a large copy for a subscriber that usually wants
 * one alias -- so read what you need back from the store. */
typedef void (*wfc_on_group_members_update_t)(const char *group_id, size_t n,
                                              void *ud);

/* The friend list changed; `n` entries were touched. Same reasoning as group
 * members: query the store. */
typedef void (*wfc_on_friend_list_update_t)(size_t n, void *ud);

/* The friend-request list changed; `n` entries were touched. Same reasoning
 * again: query the store. */
typedef void (*wfc_on_friend_request_update_t)(size_t n, void *ud);

/* The account's user settings changed; `n` entries were touched. Raised both
 * when UG delivers a batch and when this client's own UP is acknowledged, so
 * a screen showing a setting does not need to know which happened.
 *
 * A setting that pins or mutes a conversation ALSO raises a
 * conversation-update for that conversation, so a list that already redraws
 * on that event needs nothing from this one. */
typedef void (*wfc_on_user_settings_update_t)(size_t n, void *ud);

/* A conference event the server pushed on CONFN: a participant published or
 * unpublished, someone joined or left, the room was destroyed.
 *
 * `event` is Janus JSON, NUL-terminated, and dies with the call -- it is the
 * decoded body of the IDBuf the push carried, pointing into the packet.
 * Parse what you need inside the callback.
 *
 * This is the one event with no local state behind it. Everything else here
 * is "the store changed, go and read it"; a conference event is a wire
 * message the AV SDK's state machine consumes directly, because there is no
 * store for a call in progress. */
typedef void (*wfc_on_conference_event_t)(const char *event, void *ud);

/* ---------------------------------------------------------- subscribing */

/* Each returns NULL only if it cannot allocate, which on this board means
 * something has gone badly wrong; a caller that ignores the handle is fine,
 * a caller that stores it should not assume it is non-NULL. */

wfc_subscription_t *wfc_on_connection_status(wfc_on_connection_status_t cb, void *ud);
wfc_subscription_t *wfc_on_receive_messages(wfc_on_receive_messages_t cb, void *ud);
wfc_subscription_t *wfc_on_send_result(wfc_on_send_result_t cb, void *ud);
wfc_subscription_t *wfc_on_recall_message(wfc_on_recall_message_t cb, void *ud);
wfc_subscription_t *wfc_on_conversation_update(wfc_on_conversation_update_t cb,
                                               void *ud);
wfc_subscription_t *wfc_on_user_infos_update(wfc_on_user_infos_update_t cb, void *ud);
wfc_subscription_t *wfc_on_group_infos_update(wfc_on_group_infos_update_t cb, void *ud);
wfc_subscription_t *wfc_on_group_members_update(wfc_on_group_members_update_t cb,
                                                void *ud);
wfc_subscription_t *wfc_on_friend_list_update(wfc_on_friend_list_update_t cb, void *ud);
wfc_subscription_t *wfc_on_friend_request_update(wfc_on_friend_request_update_t cb,
                                                 void *ud);
wfc_subscription_t *wfc_on_user_settings_update(wfc_on_user_settings_update_t cb,
                                                void *ud);
wfc_subscription_t *wfc_on_conference_event(wfc_on_conference_event_t cb, void *ud);

/* Safe on NULL, safe from inside the callback being cancelled, and safe to
 * call twice only in the sense that the second call must not pass the same
 * pointer again -- the handle is dead after the first. */
void wfc_unsubscribe(wfc_subscription_t *sub);

/* Cancels each and NULLs the entries, so an array of subscriptions held by a
 * screen can be torn down in one line. This is offAll() from
 * wfcEventSubscriber.uts, and it exists for the same reason. */
void wfc_unsubscribe_all(wfc_subscription_t **subs, size_t n);

/* Cancels everything. For a shutdown path, not for ordinary use -- it also
 * cancels subscriptions belonging to code that has no idea. */
void wfc_event_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WFC_EVENT_H */
