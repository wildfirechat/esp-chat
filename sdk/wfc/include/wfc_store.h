/* The local store: what the client remembers between one packet and the next,
 * and -- depending on how it is built -- between one boot and the next.
 *
 * The whole of it is one idea: the business layer above (wfc_impl.c) never
 * learns which backend it is talking to. Two are compiled, one at a time,
 * chosen by Kconfig:
 *
 *   CONFIG_WFC_STORE_SQLITE   a database on the FATFS "storage" partition.
 *                             Heads and messages survive a power cut, so a
 *                             reconnect asks the server for a delta.
 *   CONFIG_WFC_STORE_RAM      the same API over a bounded ring in PSRAM.
 *                             Nothing is written to flash and nothing
 *                             survives a reboot -- which is exactly the P2
 *                             behaviour, now reachable as a configuration
 *                             rather than as a missing feature.
 *
 * Picking RAM leaves SQLite, its VFS and the FATFS mount out of the build
 * entirely: about 700 KB of flash and the whole storage partition. That is
 * the point of the split -- a board that only needs to show live messages
 * should not carry a database engine.
 *
 * The two differ in exactly one visible way, and it is deliberate:
 * wfc_store_is_persistent(). Callers use it to explain themselves ("head
 * restored" versus "starting fresh"), never to branch on behaviour.
 *
 * ------------------------------------------------------------------------
 * Threading: every function here takes the store's own mutex, so any task may
 * call any of them. The mutex is recursive, so a query callback may read the
 * store back; what it must not do is WRITE to it, which would invalidate the
 * walk under way. Nor should it block -- it is holding off every other task
 * that wants the store.
 *
 * Lifetimes: wfc_store_put_message() copies everything it keeps, so the
 * caller's wfc_message_t may point into a decoded packet as usual
 * (wfc_model.h). Going the other way, a wfc_message_t handed to a query
 * callback is valid for that call only.
 *
 * Everything else here -- conversations, users, groups, members, friends,
 * friend requests, user settings -- is copied in and copied out, so those
 * callers own what they get and can keep it. See the lifetime note in
 * wfc_model.h for why the two differ.
 */

#ifndef WFC_STORE_H
#define WFC_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- lifecycle */

typedef struct {
    /* The account this store belongs to. A store opened for a different user
     * ID than the one it was written with is wiped rather than merged: the
     * heads in it index another account's timeline, and pulling from them
     * would silently skip messages. */
    const char *user_id;
} wfc_store_config_t;

/* Mounts (SQLite) or allocates (RAM) and makes the store usable. Idempotent
 * in the sense that a second call with the same user is a no-op.
 *
 * A SQLite store that cannot be mounted or opened is a hard error: falling
 * back to RAM would look like it worked and then lose the head on every
 * reboot. The caller decides what to do about it. */
esp_err_t wfc_store_open(const wfc_store_config_t *cfg);
void      wfc_store_close(void);

bool wfc_store_is_open(void);

/* True when this build writes to flash. Compile-time constant in practice;
 * a function so callers need no #ifdef. */
bool wfc_store_is_persistent(void);

/* "sqlite" or "ram" -- for the status panel and the boot log. */
const char *wfc_store_name(void);

/* Throws away every message and every key. Used when the account changes,
 * and available to the app as a factory reset. */
esp_err_t wfc_store_clear(void);

/* ---------------------------------------------------------- key / value */

/* The sync heads. Each is a server-assigned monotonic version number, and
 * holding one across a reboot is what turns a reconnect into a delta instead
 * of a re-download -- the whole point of P3.
 *
 * P3 wrote only WFC_KEY_MSG_HEAD; P4 added WFC_KEY_FRIEND_HEAD,
 * WFC_KEY_FRIEND_RQ_HEAD came with FRP, WFC_KEY_SETTING_HEAD with UG, and
 * WFC_KEY_RECV_HEAD / WFC_KEY_READ_HEAD with the two receipt lists. Those
 * five are one mechanism written once -- see the sync table in wfc_impl.c --
 * and the version each asks from is THIS key and nothing else. It is not
 * recomputed from the rows: user settings are a list this client also writes
 * to, so the newest update_dt in the table can be a stamp we put there
 * ourselves, which would run the head past the server's and skip whatever
 * another device changed in between. The two receipt lists make the same
 * point from the other side -- their answers carry their own `current`, so
 * there is nothing in the rows to derive a version from at all.
 *
 * WFC_KEY_GROUP_CONV_HEAD is named here because it arrives in the same
 * CONNACK (wfc_mqtt.h) and belongs to a list this client does not sync yet --
 * the super-group per-conversation sync (ASSESSMENT.md section 7). */
#define WFC_KEY_MSG_HEAD        "msg_head"
#define WFC_KEY_FRIEND_HEAD     "friend_head"
#define WFC_KEY_FRIEND_RQ_HEAD  "friend_rq_head"
#define WFC_KEY_SETTING_HEAD    "setting_head"
#define WFC_KEY_RECV_HEAD       "recv_head"
#define WFC_KEY_READ_HEAD       "read_head"
#define WFC_KEY_GROUP_CONV_HEAD "group_conv_head"

#define WFC_STORE_KEY_MAX 24

esp_err_t wfc_store_set_i64(const char *key, int64_t value);

/* Returns `fallback` when the key was never written -- including on every
 * boot of a RAM store, which is how "no head yet" reaches the caller without
 * a separate not-found path. */
int64_t wfc_store_get_i64(const char *key, int64_t fallback);

/* ---------------------------------------------------------------- messages */

/* True if this message UID is already stored. This is the deduplicator: a
 * message can arrive twice, once pushed on MS and once in the MP batch that
 * follows, and both carry the same server-assigned UID.
 *
 * Only messages that were actually stored answer true -- see
 * wfc_store_put_message() on persist_flag. */
bool wfc_store_has_message(int64_t message_uid);

/* Stores one message, copying every string out of the caller's buffer.
 *
 * Returns ESP_OK when it was stored, ESP_ERR_INVALID_STATE when it was
 * deliberately not: a message whose content.persist_flag has bit 0 clear is
 * transient (a typing indicator, say) and every other client drops it too
 * (proto2 stn_callback.cc:976). Neither is a failure the caller must handle.
 *
 * A UID already present is left as it was and answers ESP_OK.
 *
 * Oldest messages are evicted past CONFIG_WFC_STORE_MAX_MESSAGES. The bound
 * is on the store as a whole, not per conversation. */
esp_err_t wfc_store_put_message(const wfc_message_t *msg);

/* How many messages are held right now. */
uint32_t wfc_store_message_count(void);

/* Return false to stop the walk early. */
typedef bool (*wfc_store_message_cb_t)(const wfc_message_t *msg, void *ud);

/* Walks stored messages newest first, up to `limit` of them.
 *
 * `conv` NULL walks every conversation, which is what the boot-time replay
 * of the message log wants; a non-NULL one filters on (type, target, line).
 *
 * Note the order: newest first is what a query wants, but the panel wants to
 * print oldest first, so the caller collects and reverses. */
esp_err_t wfc_store_query_messages(const wfc_conversation_t *conv, size_t limit,
                                   wfc_store_message_cb_t cb, void *ud);

/* One message by its server UID. False when nothing is held under it, in
 * which case `cb` is not called; `cb` runs at most once, so its return value
 * is ignored.
 *
 * The counterpart to wfc_store_has_message(), which asks the same question
 * about the same unique index and answers only yes or no. A caller that held
 * on to a UID -- and a UID is the one thing about a message that is worth
 * holding on to, being stable across repaints and across a reboot -- should
 * not have to walk a conversation to find the row again.
 *
 * `msg` BORROWS, exactly as it does in a walk: every string dies with the
 * callback (wfc_model.h), so this is where anything worth keeping is copied.
 * That is what makes this the right way to reach a field too long to cache --
 * a media URL is up to CONFIG_WFC_STORE_MAX_TEXT and the store is where the
 * whole of it lives. */
bool wfc_store_get_message(int64_t message_uid, wfc_store_message_cb_t cb, void *ud);

/* ----------------------------------------------------------- conversations */

/* The conversation list is not a thing callers maintain: it is a projection
 * of the message table, and wfc_store_put_message() keeps it up to date. A
 * message that is actually inserted moves its conversation to the top, leaves
 * its digest on the row, and -- when it is one we received and its
 * persist_flag says it counts -- bumps the unread count.
 *
 * Doing it here rather than in the business layer is what keeps the two
 * backends in step, and it makes the invariant easy to state: the list says
 * what the store holds. A duplicate (the same message pushed on MS and then
 * pulled again by MP) inserts nothing and so counts nothing, which is the
 * whole reason the insert has to report whether it happened.
 *
 * Conversations past CONFIG_WFC_STORE_MAX_CONVERSATIONS are dropped oldest
 * first, by last-traffic time. */

typedef bool (*wfc_store_conversation_cb_t)(const wfc_conversation_info_t *info, void *ud);

/* Walks the list newest first -- which is the order it is drawn in, so unlike
 * the message query the caller does not have to reverse it. */
esp_err_t wfc_store_query_conversations(size_t limit, wfc_store_conversation_cb_t cb,
                                        void *ud);

/* Copies one row out. False when the conversation has no traffic yet, leaving
 * `out` untouched. */
bool wfc_store_get_conversation(const wfc_conversation_t *conv,
                                wfc_conversation_info_t *out);

uint32_t wfc_store_conversation_count(void);

/* Sum of every conversation's unread and mention counts -- the badge. */
uint32_t wfc_store_total_unread(void);

/* Marks a conversation read: both counters to zero. Purely local -- telling
 * the server, so the other end and this account's other devices agree, is
 * wfc_clear_unread() in wfc_client.h, which calls this and then reports. */
esp_err_t wfc_store_clear_unread(const wfc_conversation_t *conv);

/* Forgets a conversation and the messages in it. Used when the server says a
 * group is gone; also what a UI "delete" would call. */
esp_err_t wfc_store_remove_conversation(const wfc_conversation_t *conv);

/* -------------------------------------------------------------- profiles */

/* Caches for the three things a screen needs to turn IDs into names: users,
 * groups, and per-group member aliases. All three are refreshable from the
 * server at any time, so they are caches in the strict sense -- losing one
 * costs a round trip, never data.
 *
 * Each table is bounded (CONFIG_WFC_STORE_MAX_PROFILES per table,
 * CONFIG_WFC_STORE_MAX_GROUP_MEMBERS per group) and evicts oldest-cached
 * first. A board talks to a handful of people, so in practice nothing is ever
 * evicted; the bound is there so a large deployment cannot fill the flash
 * with profiles nobody will look at.
 *
 * The put functions overwrite by ID and are the only way rows appear. */

esp_err_t wfc_store_put_user(const wfc_user_info_t *user);

/* False when the user is not cached, leaving `out` untouched. The caller
 * decides whether that is worth a UPUI -- wfc_client.h's
 * wfc_get_user_info() is the one that does. */
bool wfc_store_get_user(const char *user_id, wfc_user_info_t *out);

esp_err_t wfc_store_put_group(const wfc_group_info_t *group);
bool      wfc_store_get_group(const char *group_id, wfc_group_info_t *out);

/* Merges a GPGM answer in. Each member carries its own group_id, and the
 * server sends every member changed since the head we asked from -- so this
 * is an update, not a replacement, and a member whose type is Removed is a
 * removal rather than a row to keep.
 *
 * Handling the whole batch in one call lets the SQLite backend wrap it in a
 * single transaction -- one journal write for a 200-member group instead of
 * two hundred. */
esp_err_t wfc_store_put_group_members(const wfc_group_member_t *members, size_t n);

bool wfc_store_get_group_member(const char *group_id, const char *member_id,
                                wfc_group_member_t *out);

typedef bool (*wfc_store_group_member_cb_t)(const wfc_group_member_t *member, void *ud);

esp_err_t wfc_store_query_group_members(const char *group_id, size_t limit,
                                        wfc_store_group_member_cb_t cb, void *ud);

/* Newest update_dt held for this group, which is the head the next GPGM asks
 * from. 0 when nothing is cached, which asks for the whole roster. */
int64_t wfc_store_group_member_max_dt(const char *group_id);

/* --------------------------------------------------------------- friends */

/* FP answers with everything changed since the version we sent, ended
 * relationships included. Unlike the group members below, the version to send
 * next time is not derived from these rows -- it is WFC_KEY_FRIEND_HEAD. */
esp_err_t wfc_store_put_friends(const wfc_friend_t *friends, size_t n);

bool wfc_store_get_friend(const char *user_id, wfc_friend_t *out);

typedef bool (*wfc_store_friend_cb_t)(const wfc_friend_t *entry, void *ud);

/* Current friends only: entries whose relationship ended are held so the next
 * FP does not re-deliver them, not so they can be listed. */
esp_err_t wfc_store_query_friends(size_t limit, wfc_store_friend_cb_t cb, void *ud);

/* -------------------------------------------------------- friend requests */

/* FRP behaves exactly like FP: everything changed since the version we sent,
 * answered requests included. A request is keyed on the pair
 * (from_uid, to_uid) -- the same two people can have one outstanding in each
 * direction. */
esp_err_t wfc_store_put_friend_requests(const wfc_friend_request_t *requests, size_t n);

/* One request by the pair it belongs to. False when there is none, leaving
 * `out` untouched.
 *
 * The lookup exists for the write path rather than for drawing: answering a
 * request (FHR) files the new status locally on the acknowledgement, and the
 * row has to be read before it is written back or the reason the sender typed
 * -- which is the only part of it a person reads -- would be replaced by an
 * empty string until the next FRP. */
bool wfc_store_get_friend_request(const char *from_uid, const char *to_uid,
                                  wfc_friend_request_t *out);

typedef bool (*wfc_store_friend_request_cb_t)(const wfc_friend_request_t *entry,
                                              void *ud);

/* Both directions, answered ones included -- an accepted request is still
 * something the list wants to show. In cache order, like the friends query:
 * neither backend sorts, so a caller that wants newest-first sorts what it
 * is given. */
esp_err_t wfc_store_query_friend_requests(size_t limit,
                                          wfc_store_friend_request_cb_t cb, void *ud);

/* --------------------------------------------------------- user settings */

/* The third head-driven list, and the only one with a write path: UG fills
 * this table, UP adds one row to it, and both go through here.
 *
 * Rows are keyed on (scope, key) and are kept whatever their scope, including
 * the ones this client has no idea about -- they belong to the account, not
 * to the board, and a client that dropped them would show a different picture
 * from the phone next to it after a round trip.
 *
 * Two conversation scopes are more than storage: putting a
 * WFC_SETTING_CONVERSATION_TOP or _SILENT row updates the conversation row it
 * names, the same way putting a message does. That is why the projection
 * lives here rather than in the business layer -- one place, both backends,
 * and the list cannot disagree with the settings that produced it. */
esp_err_t wfc_store_put_user_settings(const wfc_user_setting_t *settings, size_t n);

/* False when the account has no such setting, leaving `out` untouched. */
bool wfc_store_get_user_setting(int32_t scope, const char *key,
                                wfc_user_setting_t *out);

typedef bool (*wfc_store_user_setting_cb_t)(const wfc_user_setting_t *entry, void *ud);

/* Every setting in `scope`, or every setting held when `scope` is
 * WFC_SETTING_SCOPE_ANY. In cache order, like the friends query. */
esp_err_t wfc_store_query_user_settings(int32_t scope, size_t limit,
                                        wfc_store_user_setting_cb_t cb, void *ud);

/* --------------------------------------------------------------- receipts */

/* The fourth and fifth head-driven lists: RCP's deliveries and RDP's reads
 * (wfc_model.h says what each one means and why they are shaped differently).
 *
 * Both puts take the LATER of the stored value and the new one rather than
 * overwriting. The server's clocks only go forward, but a delta that arrives
 * out of order -- a reconnect racing a push -- would otherwise walk a receipt
 * backwards and un-tick a message that was already read.
 *
 * Neither table is a projection onto anything: a receipt is read where it is
 * drawn, by comparing it against a message's timestamp, so unlike the pinned
 * and muted settings there is no conversation row to keep in step. Both are
 * bounded by CONFIG_WFC_STORE_MAX_PROFILES, which is the same scale -- a row
 * per person talked to, and per conversation for reads. */

esp_err_t wfc_store_put_deliveries(const wfc_delivery_t *entries, size_t n);

/* 0 when nothing is held for this user, which reads the same as "nothing of
 * ours has reached them yet" and is what the caller wants either way. */
int64_t wfc_store_delivery_dt(const char *user_id);

esp_err_t wfc_store_put_reads(const wfc_read_entry_t *entries, size_t n);

int64_t wfc_store_read_dt(const wfc_conversation_t *conv, const char *user_id);

typedef bool (*wfc_store_read_cb_t)(const wfc_read_entry_t *entry, void *ud);

/* Everyone whose read mark this store holds for `conv`. A single chat has at
 * most one; a group has one per member who has read anything, which is what
 * "read by 3" counts. In cache order, like the friends query. */
esp_err_t wfc_store_query_reads(const wfc_conversation_t *conv, size_t limit,
                                wfc_store_read_cb_t cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* WFC_STORE_H */
