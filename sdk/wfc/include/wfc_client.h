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
 * What this client syncs, and what it does not.
 *
 *   messages      MS / MP / MN, since P2
 *   conversations a projection of the stored messages, with unread counts
 *   profiles      UPUI users, GPGI groups, GPGM group members, on demand
 *   friends       FP, from the CONNACK's friend_head and the FN push
 *   friend rqs    FRP, from the CONNACK's friend_rq_head and the FRN push
 *   settings      UG / UP, and with them conversation pinning and muting
 *   receipts      RCP / RDP inbound, and the conversation-sync setting
 *                 outbound; only where the deployment has the feature bit
 *
 * And what it can change, as opposed to read: the account's settings (UP),
 * how far it has read (the same, scope 7), friendship (FAR / FHR / FDL /
 * FALS) and group membership (GC / GAM / GKM / GQ).
 *
 * Not yet, and each for a stated reason: the super-group conversation sync
 * (GCP / GMP, a second head per group that ordinary groups do not use),
 * dismissing a group and renaming one (GD / GMI -- one account owns a group
 * and this board is unlikely to be it), managers and mutes (GSM / GMM), and
 * the black list. ASSESSMENT.md section 7 has the full list.
 *
 * ------------------------------------------------------------------------
 * Reconnection is here, and it is the client's own business.
 *
 * A dropped link is retried with a backoff -- 2 s, doubling, capped at a
 * minute, jittered -- until it comes back or until the application says
 * stop. The application sees it happen and nothing more: the status goes
 * UNCONNECTED, then CONNECTING on each attempt, then CONNECTED, and the
 * catch-up that follows is the ordinary one, a delta from the stored head.
 *
 * Five answers are not retried, because repeating the request cannot change
 * them: a token the server will not accept, a token that is not this client
 * ID's, an account it has blocked, a deployment whose licence will not have
 * us, and ROFL -- another client took the session, and dialling again would
 * take it back off them. Those stop the retry loop and stay on the status, so
 * a screen can say which one happened; calling wfc_client_connect() again is
 * then an explicit decision, which is the point.
 *
 * WFC_STATUS_TIME_INCONSISTENT is the one refusal that IS retried, and it is
 * a deliberate departure from the phone clients. A board's clock is wrong for
 * one reason -- SNTP has not landed yet -- and it fixes itself a few seconds
 * later, so a client that gave up on the first attempt would need a power
 * cycle to recover from a race it was always going to win.
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

/* connectionStatus.js, value for value -- an application that already knows
 * WFC's numbers can compare against them, and a status this client does not
 * produce is a hole in the list rather than a number that means something
 * else here. Negative is a failure.
 *
 * Most of them come from /route's status byte rather than from the long
 * link (wfc_route.h), which is the same place WFC.js gets them
 * (wfcImpl.js:843-856) and proto2 does (business.cc:585-597). */
typedef enum {
    WFC_STATUS_TIME_INCONSISTENT   = -9, /* our clock is too far from the server's */
    WFC_STATUS_NOT_LICENSED        = -8, /* the licence will not have this client */
    WFC_STATUS_KICKED_OFF          = -7, /* another client took the session */
    /* The token is not this client ID's -- the first thing to check when a
     * board that worked yesterday stops connecting, since the app server
     * issues the two together and a token pasted next to somebody else's
     * client ID looks exactly like this. */
    WFC_STATUS_SECRET_KEY_MISMATCH = -6,
    WFC_STATUS_TOKEN_INCORRECT     = -5,
    WFC_STATUS_SERVER_DOWN         = -4, /* /route or the long link would not come up */
    WFC_STATUS_REJECTED            = -3, /* the account is blocked, or CONNECT refused */
    WFC_STATUS_LOGOUT              = -2, /* disconnect(); nothing is retrying */
    WFC_STATUS_UNCONNECTED         = -1,
    WFC_STATUS_CONNECTING          = 0,
    WFC_STATUS_CONNECTED           = 1,  /* working; everything is available */
    /* Connected and catching up. Messages are arriving in batches and the
     * conversation list is still moving, so a UI can say so rather than
     * flickering through a hundred redraws. */
    WFC_STATUS_RECEIVING           = 2,
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
     * roaming history the server kept.
     *
     * true is what an application with a conversation list wants. There is no
     * conversation list on the wire to fetch -- the list is a projection of
     * the message table, maintained as each message is stored -- so a login
     * that pulls nothing has no conversations either, and the list fills only
     * when somebody sends something. */
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

/* Route, connect, authenticate, then start the catch-up -- and from here on,
 * keep the link up.
 *
 * The one blocking call here: it returns once the CONNACK has arrived (or
 * failed), which takes a few seconds on a cold start. Everything after that
 * happens on the client's own tasks and reaches the caller as events, so this
 * is called once from a startup path and never from a callback.
 *
 * The return value is the first attempt's, and a failed first attempt is not
 * the end of it: unless the answer was one of the three that will not change
 * (see the note at the top), the retry loop is running by the time this
 * returns, and a caller that treats the error as fatal is throwing away a
 * board that would have come up on its own a few seconds later. Watch the
 * connection status instead.
 *
 * The clock must already be right. Every encrypted payload carries an "hours
 * since 2018-01-01" prefix that the server checks, so connecting before SNTP
 * has landed does not degrade -- it fails (ASSESSMENT.md risk R4). */
esp_err_t wfc_client_connect(void);

/* Stops reconnecting, sends DISCONNECT and tears the link down. Never call
 * from an event callback: it waits for the tasks those callbacks run on. */
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

/* One stored message, by the server UID a caller kept from a walk. False when
 * nothing is held under it; `cb` runs at most once and its return value is
 * ignored.
 *
 * What this is for: a screen keeps rows, not messages -- a message BORROWS
 * (wfc_model.h) and cannot outlive the callback it arrived in, so a page that
 * wants a field it did not copy has to ask again. Copying more of the message
 * into the row instead is the trap this exists to avoid: a row is fixed size
 * and rebuilt on every repaint, so a field long enough to matter (a media URL
 * runs to CONFIG_WFC_STORE_MAX_TEXT) gets clipped there, silently.
 *
 * Cheap -- it is an index lookup on the same unique index the deduplicator
 * uses -- but not free, and it takes the store's lock. Ask from somewhere
 * that can afford it: once per thing that needs one, not once per row of a
 * redraw. */
bool wfc_get_message(int64_t message_uid, wfc_store_message_cb_t cb, void *ud);

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

/* The badge: every conversation's unread and mention counts, summed.
 *
 * The count is the ACCOUNT's, not the board's: a conversation read on the
 * phone comes down here too, when the read mark that says so arrives. It
 * arrives as a conversation-update event, like every other change to a row. */
uint32_t wfc_get_unread_count(void);

/* Marks a conversation read: the counters go to zero, a conversation-update
 * event redraws the list, and -- if there was anything unread -- the server
 * is told.
 *
 * Telling the server is one request that does two jobs (see
 * WFC_SETTING_CONVERSATION_SYNC in wfc_model.h): it is this account's read
 * mark, so the phone in your pocket stops showing a badge for a conversation
 * this board has read, and it is the read receipt the senders get. The second
 * half only happens where the deployment has receipts and the account has not
 * turned them off; the first happens always.
 *
 * It does NOT block and it does not go out on the caller's task -- the report
 * is left for the link supervisor, which is what makes this safe to call
 * while a screen is being built and what makes a read survive being offline.
 * The cost of that is that it is not durable: a power cut before the link
 * comes back loses the report, and the other end goes on showing the message
 * as unread until this board opens the conversation again. */
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

/* ------------------------------------------------- changing a relationship */

/* The eight writes below are the other side of the two lists above and of the
 * group caches: everything else in this header either reads the store or asks
 * the server to refresh it, and these change what the server holds.
 *
 * They share one shape, and it is worth stating once.
 *
 *   Nothing is written locally before the server agrees. The request goes out,
 *   and the row this board keeps is filed when -- and only when -- the reply
 *   says it worked. A refused change simply does not happen, rather than
 *   appearing on screen and then being taken back. Same rule as UP.
 *
 *   The local row is usually a head start rather than the truth: the server
 *   announces the change to this account too (FN, FRN, or a group
 *   notification message) and the delta that follows overwrites it. Two are
 *   the exception, and on those the local row is all there is --
 *   wfc_send_friend_request(), which the server tells only the person being
 *   asked, and wfc_quit_group(), which it announces to the group after this
 *   account has already left it.
 *
 *   A row this client wrote itself carries version 0. update_dt on a stored
 *   profile is the server's version of it and goes straight back out in the
 *   next request; a number we invented there would make the server answer
 *   "not modified" and the real record would never arrive.
 *
 * The reply reaches `cb` on the wfc_mqtt task, so wfc_event.h's rules apply to
 * it: do not block, do not touch widgets. `error_code` is 0 for success, 222
 * when only some of a batch went through (the group ones), and otherwise a
 * server code from errorCode.js. `cb` may be NULL.
 *
 * The esp_err_t each returns says only whether the request was submitted --
 * ESP_ERR_INVALID_STATE when the link is down, ESP_ERR_INVALID_ARG on
 * nonsense, ESP_ERR_INVALID_SIZE past WFC_OP_MEMBERS_MAX. When it is not
 * ESP_OK, `cb` is not called. */
typedef void (*wfc_operation_cb_t)(int error_code, void *ud);

/* How many people one group request may name. A board builds a group out of
 * the handful of people on its contacts page; a hundred-person roster is
 * assembled somewhere with a keyboard. Past it the call is rejected rather
 * than truncated, because silently inviting eleven of twelve people is worse
 * than not inviting anyone. */
#define WFC_OP_MEMBERS_MAX 32

/* FAR. Ask to be someone's friend. `reason` is the free text they see beside
 * the request and may be NULL. */
esp_err_t wfc_send_friend_request(const char *user_id, const char *reason,
                                  wfc_operation_cb_t cb, void *ud);

/* FHR. Answer a request somebody sent us. `user_id` is the sender -- the side
 * of the pair that is not this account. Accepting makes both sides friends,
 * which arrives a moment later as a friend-list update. */
esp_err_t wfc_handle_friend_request(const char *user_id, bool accept,
                                    wfc_operation_cb_t cb, void *ud);

/* FDL. End a friendship, both ways: the server clears the relationship on
 * both accounts, so the other end stops being able to see this one too. */
esp_err_t wfc_delete_friend(const char *user_id, wfc_operation_cb_t cb, void *ud);

/* FALS. Set what I call them, which wins over their nickname everywhere a
 * name is drawn (wfc_get_display_name). "" clears it.
 *
 * Note the topic: FALS is the alias, and FAR is the friend request. The two
 * carry the same AddFriendRequest protobuf, which is why they are easy to
 * mistake for each other. */
esp_err_t wfc_set_friend_alias(const char *user_id, const char *alias,
                               wfc_operation_cb_t cb, void *ud);

/* ------------------------------------------------------- group management */

/* GC. Create a group and get its ID back.
 *
 * The server assigns the ID, adds this account as the owner whether or not it
 * is in `members`, and posts the "created the group" notification itself --
 * which is why nothing here says anything about a notification. A client that
 * sent its own would be refused outright on any deployment that has not
 * turned on custom group notifications (CreateGroupHandler.java:37).
 *
 * `cb` gets the new group ID, valid for that call only. The group is in the
 * store by the time it runs, so wfc_get_group_info() answers immediately;
 * a conversation row appears when the notification message lands. */
typedef void (*wfc_create_group_cb_t)(int error_code, const char *group_id,
                                      void *ud);

esp_err_t wfc_create_group(const char *name, const char **members, size_t n_members,
                           wfc_create_group_cb_t cb, void *ud);

/* GAM / GKM. Invite people in, or remove them.
 *
 * Who is allowed to do which is the server's business and depends on the
 * group's type and on whether this account owns or manages it -- a refusal
 * comes back as an error code rather than being predicted here. */
esp_err_t wfc_add_group_members(const char *group_id, const char **members,
                                size_t n_members, wfc_operation_cb_t cb, void *ud);
esp_err_t wfc_kick_group_members(const char *group_id, const char **members,
                                 size_t n_members, wfc_operation_cb_t cb, void *ud);

/* GQ. Leave a group.
 *
 * This board also drops the conversation and the messages in it, which is a
 * departure from WFC.js -- see ASSESSMENT.md section 8.13. A row that cannot
 * be written to and will never receive anything again is not one to keep on a
 * list of sixty-four. Subscribers hear about it as a conversation-removed
 * event (wfc_event.h). */
esp_err_t wfc_quit_group(const char *group_id, wfc_operation_cb_t cb, void *ud);

/* ------------------------------------------------------------------ locks */

/* SLT. The deployment's one distributed lock, held on the server and named by
 * a string both ends agree on.
 *
 * It is here rather than in whatever feature wants it because that is where
 * every other client keeps it -- WFC.js has requireLock() on the client
 * object (wfcImpl.js:4800) and Android has it on ChatManager -- and because
 * nothing about it is specific to one: it is a `putIfAbsent` on a map with an
 * expiry (MemoryMessagesStore.java:9264), and what it means is entirely up to
 * the two sides that pick the same name.
 *
 * The one caller in this tree is push-to-talk, which uses it as the microphone
 * of a two-person channel: whoever gets the lock talks and the other is told
 * the channel is busy.
 *
 * `duration_s` is how long the server keeps it before it expires on its own.
 * That expiry is the whole safety net -- a client that crashes mid-hold does
 * not lock a channel forever -- so it should be a few seconds, not a few
 * minutes, and a holder that needs longer asks again (a repeat from the same
 * account refreshes rather than fails).
 *
 * Error codes worth naming, from ErrorCode.java:
 *
 *   25  someone else holds it   (require)
 *   26  it is not yours to drop (release)
 *
 * Same shape as the eight writes above: ESP_OK means the request went out,
 * `cb` runs on the wfc_mqtt task with the server's answer, and nothing is
 * kept locally either way -- there is no local lock table, because a lock
 * whose holder this board guessed at would be worse than asking. */

/* Long enough for "WFPTT_" and two user IDs, which is the longest name this
 * tree builds. Past it the call is rejected: half a lock ID names a different
 * lock, and taking the wrong one looks exactly like taking the right one. */
#define WFC_LOCK_ID_MAX 96

esp_err_t wfc_require_lock(const char *lock_id, int32_t duration_s,
                           wfc_operation_cb_t cb, void *ud);
esp_err_t wfc_release_lock(const char *lock_id, wfc_operation_cb_t cb, void *ud);

/* --------------------------------------------------------------- receipts */

/* How far the other side has got. Reads answer from the store like every
 * other read here; what fills the store is RCP and RDP, which are only synced
 * where the deployment has the feature (wfc_route.h) -- so on a server
 * without it every one of these answers "nothing yet", which is the honest
 * answer and needs no branch at the call site.
 *
 * The two lists are shaped differently and wfc_model.h says why: a delivery
 * is one clock per PERSON, a read is one per person per conversation. */

/* True when a receipt means anything here: the deployment has the feature and
 * the account has not turned receipts off. A screen can use it to leave the
 * whole column out rather than drawing ticks that will never fill in. */
bool wfc_is_receipt_enabled(void);

/* 0 when nothing is held, which reads as "not yet" and is what a caller
 * wants either way. */
int64_t wfc_get_delivery(const char *user_id);
int64_t wfc_get_read(const wfc_conversation_t *conv, const char *user_id);

/* Everyone whose read mark this store holds for `conv`. */
esp_err_t wfc_get_reads(const wfc_conversation_t *conv, size_t limit,
                        wfc_store_read_cb_t cb, void *ud);

/* What to draw against one message we sent, in a SINGLE chat: the two lookups
 * above against its timestamp, in the order that matters (read implies
 * delivered, so read wins).
 *
 * A group answers WFC_RECEIPT_SENT, deliberately -- "delivered" has no single
 * meaning across twenty people, and what a group wants is a number.
 * wfc_message_read_count() is that number. */
typedef enum {
    WFC_RECEIPT_SENT      = 0,   /* the server has it; nobody has it yet */
    WFC_RECEIPT_DELIVERED = 1,
    WFC_RECEIPT_READ      = 2,
} wfc_receipt_t;

wfc_receipt_t wfc_message_receipt(const wfc_conversation_t *conv, int64_t timestamp);

/* How many people have read as far as `timestamp`. Works for both kinds of
 * conversation; a single chat answers 0 or 1. */
size_t wfc_message_read_count(const wfc_conversation_t *conv, int64_t timestamp);

/* --------------------------------------------------------- user settings */

/* The account's settings, which are the account's and not the board's: what
 * it has pinned, what it has muted, and whatever else the phone or the
 * desktop client has set. UG pulls them, UP changes one, and every device
 * logged in sees the change.
 *
 * Reads answer from the store and never fetch, like every other read here.
 * A setting that has never been set is simply absent -- false, and `buf`
 * empty -- which is how a default reaches the caller with no second path.
 *
 * The write does not wait: it sends and returns, and the local row appears
 * when the server acknowledges it, at which point a user-settings-update
 * event says so. A rejected change therefore does nothing at all, which is
 * the honest outcome -- a screen that showed it and then took it back would
 * be worse. ESP_OK here means "sent", not "set".
 *
 * NOT safe from an LVGL callback: it reaches a blocking send() on the long
 * link (see the threading note in ui_page.h on the application side). */
bool wfc_get_user_setting(int32_t scope, const char *key, char *buf, size_t buf_size);

esp_err_t wfc_get_user_settings(int32_t scope, size_t limit,
                                wfc_store_user_setting_cb_t cb, void *ud);

esp_err_t wfc_set_user_setting(int32_t scope, const char *key, const char *value);

/* Pinned and muted, the two settings that are about a conversation. Both go
 * through wfc_set_user_setting() with the key WFC builds from (type, line,
 * target); the conversation's row picks the change up when the server
 * acknowledges it, and the list re-sorts on the conversation-update event
 * that follows. Same threading rule as above. */
esp_err_t wfc_set_conversation_top(const wfc_conversation_t *conv, bool top);
esp_err_t wfc_set_conversation_silent(const wfc_conversation_t *conv, bool silent);

#ifdef __cplusplus
}
#endif

#endif /* WFC_CLIENT_H */
