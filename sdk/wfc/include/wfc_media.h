/* Putting a file where a message can point at it.
 *
 * Everything else in this client is one round trip on the long link. An
 * upload is three steps and only the first of them is: ask the server where
 * to put it, PUT or POST the bytes at whatever HTTP service it named, then
 * hand the resulting URL to wfc_send_message() as remote_media_url. The file
 * never goes over the long link.
 *
 * ------------------------------------------------------------------------
 * There are two ways to ask, and which one applies is the server's to say.
 *
 * GMPU is asked first. It answers with a URL the server has already signed:
 * nothing is left to compute, which is why it is the path that works against
 * object stores this board will never carry code for. Its ONE refusal,
 * ERROR_CODE_NOT_IMPLEMENT, means precisely "this deployment stores media
 * itself" -- the handler's first line -- and that is the case GMUT exists
 * for, so a refusal is a redirection rather than a failure.
 *
 * GMUT is the other way: a token to build a request WITH. Only its
 * self-hosted branch is implemented here, because it is the only one that
 * does not need the client to know an encoding of its own, and it is the only
 * one GMPU does not cover.
 *
 * The route's WFC_COMMERCIAL_BIG_FILE_UPLOAD bit looks like it would save the
 * round trip and does not: it is a licence flag and GMPU's handler never
 * reads it. ASSESSMENT.md section 8.14 has what trusting it costs.
 *
 * ------------------------------------------------------------------------
 * Neither topic is spelled the way ASSESSMENT.md spelled it for two months.
 *
 * It said GUT, and there is no GUT. The handler is @Handler("GMUT")
 * (GetMediaUploadTokenHandler.java), the constant is
 * getMediaUploadTokenTopic in proto2 (business.cc:248), and WFC.js publishes
 * 'GMUT' (wfcImpl.js:4282); GMPU is beside it on business.cc:249. This is the
 * same shape of mistake as FALS in section 8.13: a name written down once
 * from memory and never contradicted, because nothing tries it until somebody
 * implements the feature.
 *
 * ------------------------------------------------------------------------
 * THIS CALL BLOCKS. It is an MQTT round trip followed by an HTTP one, and it
 * can take seconds on a slow link.
 *
 * So it is bound by the same rule as everything else that blocks: never from
 * a wfc callback (they run on the transport task, which is the task that
 * would have to deliver the reply this call is waiting for -- calling it
 * there deadlocks until the timeout), never from a page's create(), refresh()
 * or draw(). A task of the caller's own, or a page's prime().
 *
 * ------------------------------------------------------------------------
 * The key is not ours to choose freely.
 *
 * The server checks it (GetMediaUploadTokenHandler.java:36): the last path
 * segment must start with our user ID, or with our user ID base64'd in the
 * one dialect WFC uses -- standard base64 with "+" "/" "=" rewritten to
 * "-2B" "-2F" "-3D". A key that fails the check comes back as
 * ERROR_CODE_NOT_RIGHT and nothing says why. build_key() below is
 * proto2's getMediaPath() (business.cc:2186) transcribed, so it passes for
 * the same reason theirs does.
 */

#ifndef WFC_MEDIA_H
#define WFC_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* messageContentMediaType.js. The server buckets uploads by this, so it
 * decides which bucket the file lands in as well as what it is called. */
#define WFC_MEDIA_GENERAL  0
#define WFC_MEDIA_IMAGE    1
#define WFC_MEDIA_VOICE    2
#define WFC_MEDIA_VIDEO    3
#define WFC_MEDIA_FILE     4
#define WFC_MEDIA_PORTRAIT 5

/* Room for the URL that comes back. Object stores hand out signed URLs with
 * a signature, an expiry and a credential in the query string, so this is
 * sized for one of those and not for "a domain and a file name".
 *
 * It is deliberately larger than CONFIG_WFC_STORE_MAX_TEXT: a URL that is
 * clipped on the way INTO the store is a message that can never be played,
 * and the store is where the clipping would happen silently. Getting the
 * whole thing here at least lets the caller find out. */
#define WFC_MEDIA_URL_MAX 512

/* Uploads `data` and writes where it landed into `url`.
 *
 * `ext` is the file extension including the dot (".amr"), and it is not
 * cosmetic -- it is what every other client keys its player off. `mime` is
 * the Content-Type to declare; pass NULL for "application/octet-stream",
 * which is what both reference clients send for a .amr because neither has
 * an entry for it.
 *
 * Returns ESP_OK with `url` filled, or:
 *   ESP_ERR_INVALID_STATE  not connected, or called before the client is up
 *   ESP_ERR_TIMEOUT        no answer to GMPU or GMUT
 *   ESP_ERR_NOT_SUPPORTED  neither way applies: GMPU said the deployment
 *                          stores media itself and GMUT then described
 *                          storage that is not self-hosted. That is a
 *                          misconfigured server, and the log names the type
 *   ESP_FAIL               the server refused, or the HTTP upload did
 */
esp_err_t wfc_media_upload(int32_t media_type, const char *ext, const char *mime,
                           const uint8_t *data, size_t len,
                           char *url, size_t url_size);

#ifdef __cplusplus
}
#endif

#endif /* WFC_MEDIA_H */
