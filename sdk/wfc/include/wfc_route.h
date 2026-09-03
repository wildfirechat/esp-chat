/* The one HTTP request in the protocol.
 *
 * Before any MQTT, the client POSTs an encrypted RouteRequest to /route and
 * gets back the long-link address to connect to, plus the `commercial`
 * feature bitmask. Everything after this is MQTT.
 *
 * Reference: WFC.js/lib/connect/index.js:59-270.
 *
 * Wire shape, since it is not symmetric and is easy to get subtly wrong:
 *   request  headers  p / appId / appKey, plus cid and uid encrypted under the
 *                     *root* key
 *   request  body     base64(AES(IMHttpWrapper{...RouteRequest}, privateSecret))
 *   response body     [1 byte status][AES(RouteResponse, privateSecret)],
 *                     base64-wrapped only for the web/wx `p` headers -- see
 *                     WFC_ROUTE_RESPONSE_BASE64 in wfc_platform.h
 *
 * RouteRequest.host is itself root-key encrypted and must name the same host
 * that later goes into the MQTT will-topic, or the broker rejects CONNECT.
 *
 * The port this hands back depends on RouteRequest.platform: a native platform
 * gets the MQTT TCP port, WEB/WX get the WebSocket port. wfc_platform.h has
 * the long version of why that matters.
 */

#ifndef WFC_ROUTE_H
#define WFC_ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_token.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RouteResponse.commercial bits. Annotated list in WFC.js
 * lib/connect/index.js:184-206. Bit 0 gates the client entirely; the rest are
 * feature switches this client reads as it grows into them. */
#define WFC_COMMERCIAL_COMMERCIAL           0x0001 /* professional edition */
#define WFC_COMMERCIAL_RECEIPT              0x0002 /* read/delivery receipts */
#define WFC_COMMERCIAL_CLEAR_MSG_ON_KICKOFF 0x0004
#define WFC_COMMERCIAL_NEW_AUTH             0x0008 /* newer auth; picks wssPort */
#define WFC_COMMERCIAL_NO_ROAMING_CONV_LIST 0x0010
#define WFC_COMMERCIAL_KEEP_MSG_ON_DISMISS  0x0020
#define WFC_COMMERCIAL_BIG_FILE_UPLOAD      0x0040
#define WFC_COMMERCIAL_BACKUP_DOWNLOAD_URL  0x0080
#define WFC_COMMERCIAL_NO_SYNC_DRAFT        0x0100
#define WFC_COMMERCIAL_QOS0_MESSAGE         0x0200
#define WFC_COMMERCIAL_USER_ONLINE_STATE    0x0400
#define WFC_COMMERCIAL_NO_GROUP_RECEIPT     0x2000
#define WFC_COMMERCIAL_MESH                 0x8000

#define WFC_HOST_MAX 128
#define WFC_NODE_MAX 32

typedef struct {
    const char *route_host;  /* also goes into RouteRequest.host, so it has to
                              * be the name the server knows itself by */
    uint16_t    route_port;  /* 80 unless the deployment moved it */
    const char *user_id;
    const char *client_id;   /* the token is bound to this */
    const char *token;       /* base64 blob from the app server */
    const char *language;    /* e.g. "zh_CN" */
    uint32_t    timeout_ms;
} wfc_route_config_t;

typedef struct {
    char     host[WFC_HOST_MAX]; /* long-link host, usually a node subdomain */
    char     node[WFC_NODE_MAX]; /* node id; part of the MQTT will-topic */
    uint16_t long_port;          /* MQTT over TCP on the native platforms */
    uint16_t short_port;
    uint16_t wss_port;
    uint32_t commercial;
} wfc_route_result_t;

/* Runs the whole exchange: parse token, build and encrypt the request, POST,
 * decrypt and decode the response.
 *
 * `token_out` may be NULL; pass it to keep privateSecret for the MQTT CONNECT
 * that follows, so the token is not decrypted twice.
 *
 * A community-edition server is a warning, not a failure (decision D4): this
 * client is on the native TCP path, which community servers do serve. Read
 * `commercial` if you need to know which optional features exist.
 */
esp_err_t wfc_route(const wfc_route_config_t *cfg,
                    wfc_token_t *token_out,
                    wfc_route_result_t *out);

/* Human-readable one-liner for the commercial bitmask, for logging. */
void wfc_route_describe_commercial(uint32_t commercial, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* WFC_ROUTE_H */
