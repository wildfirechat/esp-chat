/* The long link: MQTT, except where it isn't.
 *
 * WFC speaks something that looks like MQTT 3.1.1 on the wire but reports
 * protocol version 6, and the difference is not cosmetic. Two packets that the
 * standard fixes at a known length carry a payload here:
 *
 *   CONNACK  [session][return code][ConnectAckPayload protobuf, PLAINTEXT]
 *   PUBACK   [message id hi][lo][error code][ciphertext]
 *
 * That second one is the whole request/response model. There are no response
 * topics: the client PUBLISHes an encrypted protobuf to a topic named after the
 * command ("MS", "MP", "UPUI", ...) and the server puts the answer in the
 * PUBACK, matched by message id. Everything the client asks for arrives this
 * way, which is why esp-mqtt cannot be used -- it drops both payloads on the
 * floor (ASSESSMENT.md section 3.1) -- and why this file exists.
 *
 * Ported from proto2's libemqtt.cc and longlink_packer.cc, with mars's
 * AutoBuffer and task machinery replaced by a plain socket and one task.
 *
 * Direction matters for encryption and is easy to get backwards:
 *
 *   C->S  CONNECT password      encrypted (session key)
 *   C->S  PUBLISH payload       encrypted
 *   S->C  CONNACK payload       PLAINTEXT protobuf
 *   S->C  PUBACK after byte 0   encrypted
 *   S->C  PUBLISH payload       PLAINTEXT protobuf
 *
 * The client never SUBSCRIBEs. The server pushes to fixed topics on its own.
 *
 * Threading: one task owns the socket and every callback below runs on it, so
 * callbacks must not block and must not call wfc_mqtt_stop(). Requests may be
 * submitted from any task.
 */

#ifndef WFC_MQTT_H
#define WFC_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ CONNACK */

/* Return codes in CONNACK byte 1. 0-5 are the standard set; 6 and 7 are WFC's
 * own (proto2 stn_callback.cc:2548). */
#define WFC_CONNACK_ACCEPTED          0
#define WFC_CONNACK_BAD_PROTOCOL      1
#define WFC_CONNACK_ID_REJECTED       2
#define WFC_CONNACK_UNAVAILABLE       3
#define WFC_CONNACK_BAD_CREDENTIALS   4
#define WFC_CONNACK_NOT_AUTHORIZED    5
#define WFC_CONNACK_UNEXPECTED_NODE   6
#define WFC_CONNACK_NO_SESSION        7

#define WFC_NODE_ID_MAX   32
#define WFC_NODE_ADDR_MAX 64

/* Everything the server volunteers at connect time.
 *
 * The *_head fields are the synchronisation model in one struct: each is a
 * monotonic version number for one list, and the client pulls whatever it is
 * behind on. Persisting them (P3) is what turns a reconnect into a delta
 * instead of a full re-download. */
typedef struct {
    int64_t msg_head;
    int64_t friend_head;
    int64_t friend_rq_head;
    int64_t join_group_head;
    int64_t setting_head;
    int64_t recv_head;
    int64_t read_head;
    int64_t group_conv_head;
    /* Server clock in milliseconds. WFC.js halves the round trip to estimate
     * the offset; message timestamps are the server's, so this is how a client
     * displays them against its own clock. */
    int64_t server_time;
    char    node_addr[WFC_NODE_ADDR_MAX];
    int32_t node_port;
    char    node_id[WFC_NODE_ID_MAX];
} wfc_connect_ack_t;

/* ----------------------------------------------------------------- replies */

/* Error codes handed to a reply callback.
 *
 * Zero and the three below it all mean the request worked; anything else is a
 * server error code from errorCode.js (6 = token incorrect, 245 = user
 * blocked, and so on). The odd ones out are worth knowing:
 *
 *   5   nothing changed since the version you sent -- a normal answer to every
 *       "pull if newer" request, usually with no data
 *   222 partial success (proto2 calls it partSuccess): some elements of a
 *       batch request were rejected
 *   255 the plaintext was deflated. Already inflated by the time you see it,
 *       so treat it exactly like 0.
 */
#define WFC_REPLY_OK           0
#define WFC_REPLY_NOT_MODIFIED 5
#define WFC_REPLY_PARTIAL      222
#define WFC_REPLY_COMPRESSED   255

/* Local failures, kept negative so they cannot collide with a server code. */
#define WFC_REPLY_TIMEOUT      (-1)
#define WFC_REPLY_DISCONNECTED (-2)
#define WFC_REPLY_MALFORMED    (-3)

static inline bool wfc_reply_is_ok(int code)
{
    return code == WFC_REPLY_OK || code == WFC_REPLY_NOT_MODIFIED ||
           code == WFC_REPLY_PARTIAL || code == WFC_REPLY_COMPRESSED;
}

/* `data` is decrypted (and inflated if it needed it) and is valid only for the
 * duration of the call. NULL with len 0 is a legitimate answer -- code 5 in
 * particular usually carries nothing. */
typedef void (*wfc_mqtt_reply_cb_t)(int error_code, const uint8_t *data, size_t len, void *ud);

/* ------------------------------------------------------------------- state */

typedef enum {
    WFC_MQTT_DISCONNECTED = 0,
    WFC_MQTT_CONNECTING,
    WFC_MQTT_CONNECTED,
} wfc_mqtt_state_t;

/* `reason` is a CONNACK code when leaving CONNECTING, and otherwise 0. */
typedef void (*wfc_mqtt_state_cb_t)(wfc_mqtt_state_t state, int reason, void *ud);

/* An unsolicited PUBLISH from the server. `topic` is NUL-terminated; the
 * payload is plaintext protobuf (or 8 big-endian bytes for the *N head
 * notifications) and is valid only for the duration of the call. */
typedef void (*wfc_mqtt_push_cb_t)(const char *topic, const uint8_t *payload,
                                   size_t len, void *ud);

/* ------------------------------------------------------------------ config */

typedef struct {
    const char *host;           /* RouteResponse.host -- the long-link node */
    uint16_t    port;           /* RouteResponse.long_port */
    const char *node;           /* RouteResponse.node; first will-topic segment */
    /* Second will-topic segment: the host the deployment knows itself by, the
     * same string that went into RouteRequest.host. Not the long-link host. */
    const char *will_host;
    const char *user_id;        /* CONNECT username */
    const char *client_id;
    const char *user_token;     /* token segment 0; encrypted into the password */
    const char *private_secret; /* token segment 1 */

    uint16_t keepalive_s;        /* 0 -> 200, matching WFC.js */
    uint32_t connect_timeout_ms; /* 0 -> 20000 */
    uint32_t request_timeout_ms; /* 0 -> 20000, matching WFC.js */

    wfc_mqtt_push_cb_t  on_push;
    void               *push_ud;
    wfc_mqtt_state_cb_t on_state;
    void               *state_ud;
} wfc_mqtt_config_t;

/* -------------------------------------------------------------------- API */

/* Copies the config, then connects on its own task: resolve, TCP, CONNECT.
 * Returns as soon as the task is running, so a failure to reach the server
 * surfaces through wfc_mqtt_wait_connected() or the state callback, not here. */
esp_err_t wfc_mqtt_start(const wfc_mqtt_config_t *cfg);

/* Blocks until the CONNACK arrives.
 *   ESP_OK                  connected; *ack filled in if non-NULL
 *   ESP_ERR_INVALID_RESPONSE  server refused; see wfc_mqtt_connack_code()
 *   ESP_ERR_TIMEOUT         no answer in time
 *   ESP_FAIL                transport gave up before the CONNACK
 */
esp_err_t wfc_mqtt_wait_connected(uint32_t timeout_ms, wfc_connect_ack_t *ack);

/* Sends DISCONNECT if the link is up, then tears the task down. Blocks until
 * the task is gone; safe to call when not started. Never call from a callback. */
void wfc_mqtt_stop(void);

bool wfc_mqtt_is_connected(void);

/* CONNACK return code of the most recent attempt, and its name. */
int         wfc_mqtt_connack_code(void);
const char *wfc_mqtt_connack_str(int code);

/* PUBLISH `pb` to `topic` and call `cb` with whatever comes back in the PUBACK.
 * `pb` is the serialized request, still plaintext -- encryption happens here.
 * The buffer is copied, so the caller may free it on return.
 *
 * `cb` is always called exactly once: with a server code, or with
 * WFC_REPLY_TIMEOUT / WFC_REPLY_DISCONNECTED. It may be NULL for fire and
 * forget, which still occupies a slot until the PUBACK or the timeout.
 *
 * Returns ESP_ERR_INVALID_STATE when not connected, ESP_ERR_NO_MEM when every
 * in-flight slot is taken, and ESP_FAIL when the write itself failed. None of
 * the three call `cb`. */
esp_err_t wfc_mqtt_request(const char *topic, const uint8_t *pb, size_t pb_len,
                           wfc_mqtt_reply_cb_t cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* WFC_MQTT_H */
