/* The vocabulary: what the four content types are numbered, what can go
 * wrong asking to talk, and why a talk ended.
 *
 * Every number here comes from a reference client and none of them may be
 * renumbered -- they are on the wire between this board and a phone. The
 * content types are pttStart / pttSoundData / pttEnd / pttSound as
 * ptt.js/ptt/internal/ registers them and as android-pttclient's @ContentTag
 * declares them; the error codes are pttErrorCode.js; the end reasons are
 * pttEndReason.js.
 *
 * There are no Chinese strings in this component, for the reason wfc-esp
 * gives about its own type table: the words a person reads belong to the
 * application. The *_str() functions below are ASCII labels for logs.
 */

#ifndef WFPTT_TYPES_H
#define WFPTT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- content types */

/* The audio itself: raw AMR-NB frames in MessageContent.data, no file
 * header, one message every WFPTT_CHUNK_MS. Transparent -- it is stored
 * nowhere and counts towards nobody's unread badge, which is what makes it
 * possible to send two or three of them a second. */
#define WFPTT_CONTENT_SOUND_DATA 21

/* "I have stopped talking." Transparent, carries nothing. */
#define WFPTT_CONTENT_END        22

/* The optional keepsake: the whole of one press-and-hold, uploaded as an
 * ordinary voice message so the conversation has a record of it. This is the
 * ONLY one of the four that is stored, and it is a SoundMessageContent in
 * everything but its type number -- remoteMediaUrl, mediaType 2,
 * {"duration":N} -- so a client that has never heard of push-to-talk still
 * plays it. */
#define WFPTT_CONTENT_SOUND      23

/* "I am about to talk", carrying the speaker's priority. Transparent.
 *
 * It is not required to hear someone: a listener that missed it starts
 * playing on the first sound-data message instead (the reference clients do
 * the same and say so). What it buys is the half second before the first
 * chunk arrives, in which a screen can already say who is talking. */
#define WFPTT_CONTENT_START      24

/* ---------------------------------------------------------------- state */

typedef enum {
    WFPTT_IDLE = 0,
    /* Asked for the channel and waiting for the server to say who got it.
     * Only reachable where the channel allows one speaker, because that is
     * the only case with a lock to wait for. */
    WFPTT_REQUESTING,
    WFPTT_TALKING,
} wfptt_state_t;

/* --------------------------------------------------------------- errors */

/* Why a request to talk was refused. pttErrorCode.js, and negative for the
 * reason it is there: a positive number in the same field is a server reply
 * code, so the two can be carried together and still be told apart. */
#define WFPTT_ERR_UNKNOWN           (-1)
/* Somebody else holds the channel. This is the lock answering, and in a
 * two-person channel it is the ordinary "the other end is talking". */
#define WFPTT_ERR_OCCUPIED          (-2)
/* As many people are already talking as this channel allows. */
#define WFPTT_ERR_MAX_SPEAKER       (-3)
#define WFPTT_ERR_GROUP_MUTED       (-4)
#define WFPTT_ERR_GROUP_MEMBER_MUTED (-5)
/* This board is already talking. */
#define WFPTT_ERR_TALKING           (-6)
#define WFPTT_ERR_NOT_IN_GROUP      (-7)
#define WFPTT_ERR_PTT_DISABLED      (-8)
/* The microphone would not open, or something else has the audio path -- a
 * call, or a voice message being recorded or played. */
#define WFPTT_ERR_RECORDER_ERROR    (-9)
/* The long link is down. Not in the reference clients, which are always
 * online by the time anything calls them; here a board on a shelf may not be,
 * and "the request never went out" is a different thing from "somebody else
 * has the channel". */
#define WFPTT_ERR_DISCONNECTED      (-10)

/* ---------------------------------------------------------- end reasons */

/* pttEndReason.js. Which of these a talk ends with is the whole of what a
 * screen has to say afterwards. */
typedef enum {
    WFPTT_END_USER_RELEASE = 0,  /* the button came up */
    WFPTT_END_TIMEOUT      = 1,  /* the cap on one press */
    WFPTT_END_TAKE_OVER    = 2,
    WFPTT_END_NETWORK      = 3,
    WFPTT_END_CHANNEL_MUTED = 4,
    WFPTT_END_MEMBER_MUTED = 5,
    WFPTT_END_MEDIA        = 6,  /* the microphone stopped */
    WFPTT_END_NOT_IN_CHANNEL = 7,
    WFPTT_END_USER_DISABLED = 8,
} wfptt_end_reason_t;

/* ---------------------------------------------------------------- names */

/* For logs. Never NULL. */
const char *wfptt_state_str(wfptt_state_t state);
const char *wfptt_error_str(int error_code);
const char *wfptt_end_reason_str(wfptt_end_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* WFPTT_TYPES_H */
