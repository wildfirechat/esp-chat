/* The model: what WFC's types look like once they are off the wire.
 *
 * These are the C shapes of the protobuf types in wfcmessage.proto -- see
 * `Message`, `MessageContent`, `Conversation`, `User`, `GroupInfo`,
 * `GroupMember` and `Friend` there -- carrying the fields this client
 * actually reads. Anything not listed stays in the protobuf and can be added
 * later without touching the wire code.
 *
 * Two lifetime conventions live here, and the difference is not an accident:
 *
 *   wfc_message_t BORROWS. Its `const char *` fields point INTO the decoded
 *   packet, so one handed to a callback is valid for the duration of that
 *   callback and not one instruction longer -- the same rule pbc imposes one
 *   layer down (wfc_pb.h). Copy what you need to keep.
 *
 *   Everything else OWNS. Profiles and conversation entries are small, fixed
 *   size and read far more often than they are written, so they are plain
 *   structs the store copies in and out. A caller can keep one on its stack
 *   and stop worrying.
 *
 * The split follows the traffic: a message is decoded once, looked at once
 * and filed; a display name is looked up on every row of every redraw.
 */

#ifndef WFC_MODEL_H
#define WFC_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------- conversations */

/* Conversation.type. A conversation is (type, target, line): the target is a
 * user ID for Single and a group ID for Group, and `line` is WFC's second
 * dimension on the same target -- 0 everywhere unless a deployment uses it. */
typedef enum {
    WFC_CONV_SINGLE   = 0,
    WFC_CONV_GROUP    = 1,
    WFC_CONV_CHATROOM = 2,
    WFC_CONV_CHANNEL  = 3,
    WFC_CONV_SECRET   = 5,
} wfc_conv_type_t;

#define WFC_TARGET_MAX 64

typedef struct {
    int  type;                      /* wfc_conv_type_t */
    char target[WFC_TARGET_MAX];
    int  line;
} wfc_conversation_t;

/* ------------------------------------------------------------- content */

/* MessageContent.type, from messageContentType.js. P2 renders Text and the
 * notifications; everything else arrives intact and shows up as its number. */
#define WFC_CONTENT_UNKNOWN            0
#define WFC_CONTENT_TEXT               1
#define WFC_CONTENT_VOICE              2
#define WFC_CONTENT_IMAGE              3
#define WFC_CONTENT_LOCATION           4
#define WFC_CONTENT_FILE               5
#define WFC_CONTENT_VIDEO              6
#define WFC_CONTENT_STICKER            7
#define WFC_CONTENT_LINK               8
#define WFC_CONTENT_P_TEXT             9
#define WFC_CONTENT_USER_CARD          10
#define WFC_CONTENT_RECALL_NOTIFY      80
#define WFC_CONTENT_TIP_NOTIFY         90
#define WFC_CONTENT_TYPING             91
/* 104..124 are the group notifications: created, member added, renamed, ... */
#define WFC_CONTENT_GROUP_NOTIFY_FIRST 104
#define WFC_CONTENT_GROUP_NOTIFY_LAST  124

/* 400..417 are the call control messages, and they are ordinary IM messages
 * -- invite, accept, bye, all carried on the same MS/MN path as a line of
 * text. That is the whole reason the AV SDK needs no transport of its own:
 * wfav subscribes to wfc_on_receive_messages() and picks these out by type.
 *
 * Their payload is base64(JSON) in MessageContent.data, which is decoded in
 * wfav-esp/components/wfav/src/wfav_msg.c -- one file per the JS reference's
 * av/messages/. They are listed here rather than there because the store
 * and the digest have to know a 400 is a call. */
#define WFC_CONTENT_VOIP_START           400  /* CallStartMessageContent  */
#define WFC_CONTENT_VOIP_ACCEPT          401  /* CallAnswerMessageContent */
#define WFC_CONTENT_VOIP_END             402  /* CallByeMessageContent    */
#define WFC_CONTENT_VOIP_SIGNAL          403  /* CallSignalMessageContent */
#define WFC_CONTENT_VOIP_MODIFY          404  /* CallModifyMessageContent */
#define WFC_CONTENT_VOIP_ACCEPT_T        405  /* CallAnswerTMessageContent */
#define WFC_CONTENT_VOIP_ADD_PARTICIPANT 406
#define WFC_CONTENT_VOIP_MUTE_VIDEO      407
#define WFC_CONTENT_VOIP_MULTI_ONGOING   416
#define WFC_CONTENT_VOIP_JOIN_REQUEST    417

/* MessageContent.persist_flag, from persistFlag.js. The sender decides it and
 * the server honours it, so a text message has to say 3 or it will not count
 * towards anyone's unread badge. */
#define WFC_PERSIST_NONE            0
#define WFC_PERSIST_PERSIST         1
#define WFC_PERSIST_PERSIST_COUNT   3
#define WFC_PERSIST_TRANSPARENT     4

typedef struct {
    int32_t     type;
    int32_t     persist_flag;
    int32_t     media_type;
    int32_t     mentioned_type;
    /* For Text this is the message body. For media types it is the digest the
     * conversation list shows. Never NULL -- "" when the field is absent. */
    const char *searchable_content;
    const char *push_content;
    const char *content;    /* type-specific, usually JSON */
    const char *remote_media_url;
    const char *extra;
    const uint8_t *data;    /* MessageContent.data, type-specific binary */
    size_t         data_len;
} wfc_message_content_t;

/* --------------------------------------------------------------- message */

typedef enum {
    WFC_DIRECTION_SEND    = 0,
    WFC_DIRECTION_RECEIVE = 1,
} wfc_direction_t;

typedef struct {
    wfc_conversation_t    conversation;
    char                  from[WFC_TARGET_MAX];
    /* The server's ID for this message, and the server's clock in ms. Both are
     * assigned by the server: on a message we sent they come back in the
     * PUBACK, and on a message we pulled they are already filled in. */
    int64_t               message_uid;
    int64_t               timestamp;
    int                   direction;   /* wfc_direction_t */
    wfc_message_content_t content;
} wfc_message_t;

/* ------------------------------------------------------------- profiles */

/* Long enough for a nickname and a portrait URL as the servers issue them.
 * A value that does not fit is truncated on a UTF-8 boundary rather than
 * rejected: a clipped name still identifies its owner, and the alternative is
 * a conversation row with no name at all. */
#define WFC_NAME_MAX 64
#define WFC_URL_MAX  128

/* A user, from the `User` protobuf. Cut down to what a client shows: WFC also
 * carries mobile, email, address, company, social and a free-text extra, none
 * of which this board displays and all of which would cost bytes in every
 * cached profile. They stay in the protobuf; add a field here when a screen
 * needs it.
 *
 * `update_dt` is the server's version of this profile, not a wall clock. It
 * goes straight back out in the next UserRequest, which is how a refresh asks
 * "anything newer than what I hold?" and usually gets told no. */
typedef struct {
    char    uid[WFC_TARGET_MAX];
    char    name[WFC_NAME_MAX];          /* login name, unique per deployment */
    char    display_name[WFC_NAME_MAX];  /* nickname, what a client shows */
    char    portrait[WFC_URL_MAX];
    int32_t type;                        /* 0 normal, 1 robot, 2 thing */
    int32_t gender;
    int64_t update_dt;
} wfc_user_info_t;

/* GroupInfo.type. */
typedef enum {
    WFC_GROUP_NORMAL     = 0,
    WFC_GROUP_FREE       = 1,
    WFC_GROUP_RESTRICTED = 2,
} wfc_group_type_t;

typedef struct {
    char    target[WFC_TARGET_MAX];   /* GroupInfo.target_id */
    char    name[WFC_NAME_MAX];
    char    portrait[WFC_URL_MAX];
    char    owner[WFC_TARGET_MAX];
    int32_t type;                     /* wfc_group_type_t */
    int32_t member_count;
    int64_t update_dt;
    /* The members' own version, which moves when someone joins, leaves or
     * renames themselves without the group itself changing. Comparing it
     * against what the member table holds is what decides whether a GPGM is
     * worth sending. */
    int64_t member_update_dt;
} wfc_group_info_t;

/* GroupMember.type. Removed is not a deletion: the server keeps the row so
 * every client learns the member left, so a roster has to filter it out. */
typedef enum {
    WFC_MEMBER_NORMAL  = 0,
    WFC_MEMBER_MANAGER = 1,
    WFC_MEMBER_OWNER   = 2,
    WFC_MEMBER_MUTED   = 3,
    WFC_MEMBER_REMOVED = 4,
    WFC_MEMBER_ALLOWED = 5,
} wfc_group_member_type_t;

typedef struct {
    char    group_id[WFC_TARGET_MAX];
    char    member_id[WFC_TARGET_MAX];
    char    alias[WFC_NAME_MAX];   /* per-group nickname; wins over every other name */
    int32_t type;                  /* wfc_group_member_type_t */
    int64_t update_dt;
} wfc_group_member_t;

/* Friend.state: 0 is a friend, anything else is a relationship that ended.
 * The server sends the ended ones too -- that is how a delete propagates --
 * so the state has to be read, not assumed. */
#define WFC_FRIEND_STATE_FRIEND 0

typedef struct {
    char    uid[WFC_TARGET_MAX];
    char    alias[WFC_NAME_MAX];   /* what I call them; wins over their nickname */
    int32_t state;
    int32_t blacked;
    int64_t update_dt;
} wfc_friend_t;

/* FriendRequest.status: 0 is the one that matters, an ask nobody has answered
 * yet. The other two are the answer, kept rather than dropped because their
 * update_dt is part of the version the next FRP asks from. */
#define WFC_FRIEND_RQ_PENDING  0
#define WFC_FRIEND_RQ_ACCEPTED 1
#define WFC_FRIEND_RQ_REJECTED 2

/* A friend request, in either direction. FRP answers with both -- an outgoing
 * one is how "they accepted" reaches the side that asked -- and the wire
 * carries no direction field, so telling them apart is a comparison of
 * `from_uid` against this client's own user ID and nothing else.
 *
 * FriendRequest also carries from_read_status / to_read_status and an extra
 * blob. Left on the wire until something draws an unread badge, per the
 * convention at the top of this file. */
typedef struct {
    char    from_uid[WFC_TARGET_MAX];
    char    to_uid[WFC_TARGET_MAX];
    char    reason[WFC_NAME_MAX];   /* free text the sender typed */
    int32_t status;
    int64_t update_dt;
} wfc_friend_request_t;

/* ---------------------------------------------------------- conversations */

/* One row of the conversation list: a conversation, when it last had traffic,
 * how much of it is unread, and enough of the last message to draw the row
 * without going back to the message table.
 *
 * The digest is stored rather than joined for two reasons. It survives
 * retention -- a quiet conversation keeps its last line long after the
 * message itself has been trimmed -- and it keeps the two store backends
 * honestly identical, since the RAM backend has no joins to do.
 *
 * `unread_mention` counts separately from `unread` and is NOT included in it,
 * matching UnreadCount in every other client: a badge shows the sum, an
 * "@ me" marker shows just the mentions. */
#define WFC_DIGEST_MAX 96

typedef struct {
    wfc_conversation_t conversation;
    int64_t            timestamp;        /* of the last message */
    uint32_t           unread;
    uint32_t           unread_mention;
    int64_t            last_message_uid;
    int32_t            last_content_type;
    int32_t            last_direction;   /* wfc_direction_t */
    char               last_from[WFC_TARGET_MAX];
    char               digest[WFC_DIGEST_MAX];
} wfc_conversation_info_t;

static inline uint32_t wfc_conversation_unread_total(const wfc_conversation_info_t *info)
{
    return info->unread + info->unread_mention;
}

/* ---------------------------------------------------------------- naming */

/* What a message type MEANS -- its name, its persist flag, whether it draws
 * as a notice, and what one line of it looks like -- is not here. It is a
 * table an application can add to, in wfc_content.h: wfc_message_digest(),
 * wfc_content_type_str() and wfc_content_is_notification() live there.
 *
 * They used to be switch statements in this file, which meant a deployment's
 * own message types could only be taught to the client by editing the
 * component. */

/* Name for a group-member type, for logs and rosters. Never NULL. */
const char *wfc_group_member_type_str(int32_t type);

/* ------------------------------------------------------------ truncation */

/* Everything the server sends is unbounded and everything this client stores
 * is not, so text gets cut -- in the store, in a conversation digest, in a
 * panel row. Cutting it in the middle of a multi-byte character is not a
 * cosmetic problem: LVGL draws the remains as a box, and the string stops
 * being valid UTF-8 for anything downstream. Chinese text lands mid-character
 * on two thirds of cut points, so this is the common case, not the corner.
 *
 * wfc_utf8_trim returns how many bytes of `src` to keep to stay within `max`
 * without splitting a sequence; wfc_copy_text does the copy and terminates. */
size_t wfc_utf8_trim(const char *src, size_t max);
void   wfc_copy_text(char *dst, size_t dst_size, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* WFC_MODEL_H */
