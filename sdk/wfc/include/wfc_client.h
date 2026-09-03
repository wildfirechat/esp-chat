/* The client: one header for anything above the protocol.
 *
 * This is WFC.js's wfc.js, and the deal is the same one every WFC client
 * offers. Reads are answered from the local store and return immediately;
 * writes and refreshes go out on the long link and come back later as events
 * (wfc_event.h). Nothing here blocks on the network except
 * wfc_client_connect(), which says so.
 *
 * That split is what makes a UI writable. Drawing a conversation row asks for
 * a display name and gets one -- the ID in angle brackets if that is all the
 * board knows yet -- and the profile arriving a moment later comes back as an
 * event that redraws the row. No screen ever waits for the server.
 *
 * ------------------------------------------------------------------------
 * What P4 syncs, and what it does not.
 *
 *   messages      MS / MP / MN, since P2
 *   conversations a projection of the stored messages, with unread counts
 *   profiles      UPUI users, GPGI groups, GPGM group members, on demand
 *   friends       FP, from the CONNACK's friend_head and the FN push
 *   friend rqs    FRP, from the CONNACK's friend_rq_head and the FRN push
 *
 * Not yet, and each for a stated reason: read and delivery receipts (RCP /
 * RDP, gated on the receipt feature bit -- second phase), user settings and
 * with them conversation pinning and muting (UG -- second phase), the
 * super-group conversation sync (GCP / GMP, a second head per group that
 * ordinary groups do not use), answering a friend request (FALS / FHR /
 * FDL -- reading the list is here, acting on it is not), and group
 * administration.
 * ASSESSMENT.md section 7 has the full list.
 *
 * Reconnection is not here either, and still is not: a dropped link stays
 * dropped and reports itself as WFC_STATUS_UNCONNECTED, and it is the
 * application that decides whether to call wfc_client_connect() again. Doing
 * it in here badly -- a tight retry against a server that is refusing the
 * token -- would be worse than not doing it (ASSESSMENT.md risk R12), and
 * doing it well means a backoff the application can see and show.
 */

#ifndef WFC_CLIENT_H
#define WFC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_content.h"
#include "wfc_event.h"
#include "wfc_model.h"
#include "wfc_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- status */

/* connectionStatus.js, kept to the values this client can actually reach.
 * Negative is a failure that will not resolve itself. */
typedef enum {
    WFC_STATUS_KICKED_OFF     = -7,  /* another client took this account's session */
    WFC_STATUS_TOKEN_INCORRECT = -5,
    WFC_STATUS_SERVER_DOWN    = -4,  /* /route or the long link would not come up */
    WFC_STATUS_REJECTED       = -3,  /* the server refused the CONNECT */
    WFC_STATUS_UNCONNECTED    = -1,
    WFC_STATUS_CONNECTING     = 0,
    WFC_STATUS_CONNECTED      = 1,   /* working; everything is available */
    /* Connected and catching up. Messages are arriving in batches and the
     * conversation list is still moving, so a UI can say so rather than
     * flickering through a hundred redraws. */
    WFC_STATUS_RECEIVING      = 2,
} wfc_connection_status_t;

const char *wfc_status_str(wfc_connection_status_t status);

/* ---------------------------------------------------------------- setup */

typedef struct {
    /* The name the deployment knows itself by: it goes into the route request
     * and later into the MQTT will topic, so it is not the per-node long-link
     * host that /route hands back. */
    const char *host;
    uint16_t    route_port;      /* 0 -> 80 */

    const char *user_id;
    const char *client_id;       /* the token is bound to this */
    const char *token;           /* the app server's base64 blob */

    /* Only consulted when the store has no message head yet -- a first boot,
     * a store wiped because the account changed, or any boot at all with
     * CONFIG_WFC_STORE_RAM. false starts from the CONNACK head, so only
     * messages sent from now on arrive; true starts from 0 and pulls whatever
     * roaming history the server kept. */
    bool pull_history;

    /* Consecutive MP round trips before the catch-up gives up and waits for
     * the next MN. 0 -> 32. */
    int max_pull_rounds;
} wfc_client_config_t;

/* Copies the config and opens the local store. Call once, before the network
 * is up -- the panel then comes up with the previous run's conversations on
 * it rather than filling in after the link does.
 *
 * A store that will not open is a hard error and is passed straight back. It
 * is not recoverable by carrying on: the sync head would stop persisting and
 * deduplication, which is "ask the store", would stop working, so every
 * pushed message would appear twice. */
esp_err_t wfc_client_init(const wfc_client_config_t *cfg);

/* Route, connect, authenticate, then start the catch-up.
 *
 * The one blocking call here: it returns once the CONNACK has arrived (or
 * failed), which takes a few seconds on a cold start. Everything after that
 * happens on the client's own task and reaches the caller as events, so this
 * is called once from a startup path and never from a callback.
 *
 * The clock must already be right. Every encrypted payload carries an "hours
 * since 2018-01-01" prefix that the server checks, so connecting before SNTP
 * has landed does not degrade -- it fails (ASSESSMENT.md risk R4). */
esp_err_t wfc_client_connect(void);

/* Sends DISCONNECT and tears the link down. Never call from an event
 * callback: it waits for the task the callback is running on. */
void wfc_client_disconnect(void);

wfc_connection_status_t wfc_client_status(void);

/* The account this client is logged in as; "" before wfc_client_init(). */
const char *wfc_client_user_id(void);

/* -------------------------------------------------------------- messages */

/* Sends a text message: wfc_send_message() with the text type and the flag
 * that makes it count towards the recipient's badge.
 *
 * Returns once the PUBLISH is on the wire; the server's answer arrives as a
 * send-result event, and the message is only stored once that answer names it
 * -- until then it has no UID to deduplicate against.
 *
 * Safe from any task, including the UI's. */
esp_err_t wfc_send_text(const wfc_conversation_t *conv, const char *text);

/* ------------------------------------------------- composed messages */

/* A message this client composes rather than types: a custom type an
 * application defined, a call invite, an accept, a bye. Same fields as
 * wfc_message_content_t plus the two only an outgoing message has, and it
 * OWNS nothing -- every pointer is the caller's and is only read for the
 * duration of the send call.
 *
 * NULL is "" everywhere. `type` and `persist_flag` are the two that matter:
 * the server honours the flag the sender puts on, so a message that says
 * WFC_PERSIST_PERSIST_COUNT lands in the recipient's unread badge, and a call
 * invite that says WFC_PERSIST_TRANSPARENT is not stored anywhere and leaves
 * no call record. WFC_PERSIST_FROM_TYPE (wfc_content.h) takes the flag from
 * the type's registration, which is what an application that registered its
 * types should use -- one table, not a flag repeated at every send site. */
typedef struct {
    int32_t        type;
    int32_t        persist_flag;
    const char    *searchable_content;
    const char    *push_content;
    /* MessageContent.push_data: what an offline push carries, which is not
     * the same thing as push_content. A call invite puts the call ID and the
     * participants here so a phone that is asleep can ring. */
    const char    *push_data;
    const char    *content;
    const char    *extra;
    const uint8_t *data;
    size_t         data_len;

    /* The media fields, for a custom type that points at something uploaded
     * rather than carrying it: mediaType is messageContentMediaType.js's
     * number and remote_media_url is where the file landed. Both are simply
     * relayed -- this client uploads nothing. */
    int32_t        media_type;
    const char    *remote_media_url;

    /* An @ mention. 1 is everyone, 2 is the users in `mentioned_targets`, and
     * it is not cosmetic: the receiving client counts a mention into
     * wfc_conversation_info_t.unread_mention rather than plain unread, which
     * is what an "@ me" marker reads. */
    int32_t        mentioned_type;
    const char *const *mentioned_targets;
    size_t         n_mentioned_targets;
} wfc_content_out_t;

/* The server's answer to one send, delivered to the caller that made it.
 *
 * The send-result EVENT (wfc_event.h) tells every subscriber that a message
 * went out, which is what a status panel wants; a state machine needs to know
 * which of its OWN sends this was -- a call invite's UID goes into the accept
 * and into the bye. So both happen: every send raises the event, and a send
 * that passed a callback also gets that callback, once, with its `ud`.
 *
 * Runs on the wfc_mqtt task, like every other reply. `error_code` is a
 * wfc_mqtt reply code: 0 is sent, and the uid and timestamp are the
 * server's. */
typedef void (*wfc_send_result_cb_t)(int error_code, int64_t message_uid,
                                     int64_t timestamp, void *ud);

/* Sends `content` to `conv`, optionally to named clients within it. This is
 * the one send path: wfc_send_text() is a wrapper, the AV SDK's signalling
 * goes out through here, and so does an application's own message type.
 *
 * `to_users` is WFC's directed message: the message goes to those user IDs
 * only, not to everyone in the conversation. Call signalling uses it for
 * everything -- an accept goes to the caller and to our own other clients,
 * not to a group -- and NULL/0 means the ordinary broadcast.
 *
 * WHAT HAPPENS LOCALLY. Once the server answers, the message is filed in the
 * store exactly as an incoming one is: the conversation row takes its digest,
 * the list moves it to the top, and a conversation-update event says so. The
 * persist flag decides, the same way it decides at the far end -- a
 * transparent call invite is stored nowhere, a custom message that says
 * persist appears in the log and survives a reboot. Nothing is stored before
 * the answer: until the server names the message it has no UID, and a
 * message with no UID cannot be deduplicated against the copy that comes back
 * on the next catch-up.
 *
 * `cb` may be NULL; the send-result event is raised either way. Safe from any
 * task.
 *
 * On a non-OK return `cb` is not called and the caller still owns `ud`. */
esp_err_t wfc_send_message(const wfc_conversation_t *conv,
                           const wfc_content_out_t *content,
                           const char **to_users, size_t n_to_users,
                           wfc_send_result_cb_t cb, void *ud);

/* ------------------------------------------------------- conference (AV) */

/* One reply to a conference request. `response` is the Janus JSON the server
 * relayed back, NUL-terminated, valid only for the duration of the call;
 * NULL when the request failed or answered with nothing.
 *
 * `error_code` is a wfc_mqtt reply code, NOT a Janus error: a request that
 * reaches Janus and is refused by it comes back as 0 with the refusal inside
 * `response` (data.error_code). The AV SDK has to read both. */
typedef void (*wfc_conference_reply_cb_t)(int error_code, const char *response,
                                          void *ud);

/* The AV control channel: PUBLISH a ConferenceRequest on the CONF topic and
 * hand the answer back.
 *
 * This is the whole of WFC's Janus signalling. The IM server proxies for
 * Janus, so a client never opens a second connection: "create_room", "join_pub",
 * "message" (which carries the SDP), "trickle", "join_sub", "keepalive" and
 * "leave" all go out here as a request name plus a JSON blob, and Janus's
 * answer comes back in the PUBACK. Server-initiated events arrive separately,
 * on the CONFN push, as wfc_on_conference_event().
 *
 * `session_id` is 0 until create_room or join_pub returns one. `data` is the
 * request's JSON, or "" for the ones that take none (keepalive). `advance` is
 * the "advanced conference" flag; a 1v1 call sets it false.
 *
 * Safe from any task. `cb` fires exactly once unless this returns non-OK. */
esp_err_t wfc_send_conference_request(int64_t session_id, const char *room_id,
                                      const char *request, const char *data,
                                      bool advance,
                                      wfc_conference_reply_cb_t cb, void *ud);

/* Stored messages, newest first, up to `limit`. `conv` NULL walks every
 * conversation. A straight pass-through to the store, here so a UI needs only
 * this header. */
esp_err_t wfc_get_messages(const wfc_conversation_t *conv, size_t limit,
                           wfc_store_message_cb_t cb, void *ud);

/* Where the catch-up has got to, and whether it is still running. */
int64_t  wfc_message_head(void);
bool     wfc_is_syncing(void);
uint32_t wfc_received_count(void);
uint32_t wfc_sent_count(void);

/* ---------------------------------------------------------- conversations */

/* The list, newest first -- the order it is drawn in. */
esp_err_t wfc_get_conversations(size_t limit, wfc_store_conversation_cb_t cb, void *ud);

bool wfc_get_conversation_info(const wfc_conversation_t *conv,
                               wfc_conversation_info_t *out);

/* The badge: every conversation's unread and mention counts, summed. */
uint32_t wfc_get_unread_count(void);

/* Marks a conversation read and raises a conversation-update event so the
 * list redraws. Local only; telling the server is the receipt path, second
 * phase. */
esp_err_t wfc_clear_unread(const wfc_conversation_t *conv);

/* -------------------------------------------------------------- profiles */

/* Answers from the cache. `refresh`, or a cache miss, also asks the server --
 * so a miss returns false now and arrives as a user-infos-update event a
 * moment later. That is the whole read model in one sentence.
 *
 * Requests are batched: several misses in the same run of callbacks become
 * one UPUI, which matters when a catch-up delivers forty messages from twenty
 * people at once. */
bool wfc_get_user_info(const char *user_id, bool refresh, wfc_user_info_t *out);

/* Same contract, GPGI. A group whose members are not cached, or whose cached
 * members are older than the group says they should be, also triggers a GPGM. */
bool wfc_get_group_info(const char *group_id, bool refresh, wfc_group_info_t *out);

esp_err_t wfc_get_group_members(const char *group_id, size_t limit,
                                wfc_store_group_member_cb_t cb, void *ud);

/* The name to draw for a user, in WFC's order of precedence:
 *
 *   group alias   what they call themselves in this group   (`group_id` set)
 *   friend alias  what I call them
 *   display name  their nickname
 *   name          their login name
 *   <user_id>     angle brackets, so an unresolved ID looks unresolved
 *
 * Always writes something. `group_id` may be NULL or "" outside a group.
 *
 * It does NOT fetch: a name that is missing is missing because the profile is
 * not cached, and this is called once per row per redraw, so making it ask
 * the server would put a UPUI behind every scroll. Call wfc_get_user_info()
 * when a screen appears to prime the cache, then let the event redraw it. */
void wfc_get_display_name(const char *user_id, const char *group_id,
                          char *buf, size_t buf_size);

/* The name to draw for a conversation: the group's name for a group, the
 * other end's display name for a single chat. Always writes something. */
void wfc_get_conversation_title(const wfc_conversation_t *conv, char *buf,
                                size_t buf_size);

/* --------------------------------------------------------------- friends */

esp_err_t wfc_get_friends(size_t limit, wfc_store_friend_cb_t cb, void *ud);

bool wfc_is_friend(const char *user_id);

/* Friend requests in both directions, answered ones included. Which side of a
 * request this client is on is `from_uid` against wfc_client_user_id(); the
 * wire carries no direction field. */
esp_err_t wfc_get_friend_requests(size_t limit, wfc_store_friend_request_cb_t cb,
                                  void *ud);

#ifdef __cplusplus
}
#endif

#endif /* WFC_CLIENT_H */
