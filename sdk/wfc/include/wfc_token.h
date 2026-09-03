/* The app-server token, and what falls out of it.
 *
 * The app server hands the client one opaque base64 blob. Decrypted with the
 * root key it is three `|`-separated segments -- the server builds them in
 * im-server's GetTokenHandler.java:38 as
 *     strToken + "|" + session.getSecret() + "|" + session.getDbSecret()
 *
 *   [0] userToken     -- the CONNECT password, re-encrypted under privateSecret
 *   [1] privateSecret -- session key for everything else on the wire
 *   [2] dbSecret      -- local database key
 *
 * WFC.js drops segment 2 because the browser keeps nothing on disk. We keep
 * it: it is the key the server already minted for our SQLite file, so P3 does
 * not have to invent one. Nothing reads it yet.
 *
 * A token is bound to the clientId it was issued for. Pairing a token with a
 * different clientId fails authentication, which is the easiest way to lose an
 * afternoon here.
 */

#ifndef WFC_TOKEN_H
#define WFC_TOKEN_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generous vs. what the server sends today (a ~56-char base64 blob and two
 * 36-char UUIDs), so a longer token is a clean error rather than a truncation. */
#define WFC_USER_TOKEN_MAX 256
#define WFC_SECRET_MAX     64

typedef struct {
    char user_token[WFC_USER_TOKEN_MAX];
    char private_secret[WFC_SECRET_MAX];
    char db_secret[WFC_SECRET_MAX];
} wfc_token_t;

/* Decrypt and split. `token_b64` is the blob exactly as the app server gave
 * it. Returns ESP_ERR_INVALID_ARG when it does not decrypt to three segments
 * -- in practice a truncated or mistyped token. */
esp_err_t wfc_token_parse(const char *token_b64, wfc_token_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WFC_TOKEN_H */
