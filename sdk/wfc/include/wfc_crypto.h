/* The wire encryption every WFC request goes through.
 *
 * AES-128-CBC with **IV == key** and PKCS7 padding, over a plaintext that
 * carries a 4-byte little-endian "hours since 2018-01-01 00:00 UTC+8" prefix.
 * Reference implementation: WFC.js/wfc/internal/aes.js.
 *
 * Two keys are in play:
 *   - the root key (fixed, below), used only to decrypt the app-server token
 *     and to encrypt the `cid` / `uid` route headers and RouteRequest.host;
 *   - the session key, the first 16 ASCII bytes of privateSecret (segment 1 of
 *     the token), used for everything else.
 *
 * The hour prefix is why the clock has to be right before connecting: the
 * server checks it. SNTP must succeed, there is no uptime fallback.
 *
 * aes.js also has a `useSM4` switch that swaps in SM4-CBC with identical
 * framing. Not implemented; the split between framing and cipher here is what
 * makes adding it a local change.
 */

#ifndef WFC_CRYPTO_H
#define WFC_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WFC_AES_KEY_LEN   16
#define WFC_AES_BLOCK_LEN 16

/* Epoch the hour prefix counts from: 2018-01-01 00:00 (UTC+8). */
#define WFC_HOUR_EPOCH 1514736000

/* 00 11 22 ... 77 78 79 7A ... 7F -- fixed, shared by every WFC client. */
extern const uint8_t wfc_root_key[WFC_AES_KEY_LEN];

/* privateSecret ("35d501ab-4f23-..." style UUID) -> the 16-byte session key.
 * Fails if the secret is shorter than 16 bytes: aes.js would silently build a
 * short key there, and silently talking to the server with the wrong key is
 * far harder to debug than refusing. */
esp_err_t wfc_key_from_secret(const char *secret, uint8_t key[WFC_AES_KEY_LEN]);

/* Hours since WFC_HOUR_EPOCH, i.e. the value that goes in the prefix. Reads
 * the system clock, so it is only meaningful after SNTP has synced. */
int32_t wfc_crypto_hour_now(void);

/* Encrypt. On success *out holds *out_len bytes; release with wfc_free().
 * with_hour_prefix prepends the 4-byte timestamp (true for every wire use;
 * false exists for the SM4/self-test paths that aes.js exposes). */
esp_err_t wfc_aes_encrypt(const uint8_t key[WFC_AES_KEY_LEN],
                          const uint8_t *in, size_t in_len,
                          bool with_hour_prefix,
                          uint8_t **out, size_t *out_len);

/* Decrypt, strip PKCS7 and (if with_hour_prefix) the 4-byte timestamp.
 * The result is NUL-terminated one byte past *out_len so it can be used as a
 * string without copying; release with wfc_free().
 *
 * A bad padding byte here almost always means the key is wrong -- for the
 * route response that is the "IM-Server is the community edition" case, which
 * WFC.js reports at exactly this point. */
esp_err_t wfc_aes_decrypt(const uint8_t key[WFC_AES_KEY_LEN],
                          const uint8_t *in, size_t in_len,
                          bool with_hour_prefix,
                          uint8_t **out, size_t *out_len);

/* base64, thin wrappers over mbedTLS. Both NUL-terminate and both hand back
 * buffers released with wfc_free(). */
esp_err_t wfc_base64_encode(const uint8_t *in, size_t in_len, char **out, size_t *out_len);
esp_err_t wfc_base64_decode(const char *in, size_t in_len, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* WFC_CRYPTO_H */
