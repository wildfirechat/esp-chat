#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_cfg.h"
#include "app_login.h"
#include "ui.h"
#include "ui_page.h"
#include "ui_setup.h"

#include "wfc_platform.h"

static const char *TAG = "app_login";

/* The server's DeferredResult gives up at 50 seconds and answers code 8, so
 * anything less than that here would turn every quiet minute into a timeout
 * that looks like an error. */
#define POLL_TIMEOUT_MS    60000

/* The session the app server mints lasts 300 seconds (AuthDataSource.java).
 * A new QR before that, rather than after, so the code on the screen is never
 * one nobody can use. */
#define SESSION_LIFE_MS    240000

/* Long enough for someone to walk back to the board with their phone. After
 * that the caller restarts: an hour-old QR code on a shelf helps nobody. */
#define GIVE_UP_MS         (60 * 60 * 1000)

/* Every answer this reads is a small JSON object: a token, or a user ID and a
 * token, or a display name. */
#define BODY_MAX           2048

/* The session key is a UUID on every deployment that ships this app server,
 * but it is the server's to choose, so there is room for something longer. */
#define APP_LOGIN_SESSION_MAX 96

/* ------------------------------------------------------------------ HTTP */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} body_t;

/* Collected through the event callback rather than with fetch_headers() and
 * read(), because Spring answers these two endpoints chunked: there is no
 * Content-Length to size a buffer from. */
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    body_t *body = evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || body == NULL || evt->data == NULL) {
        return ESP_OK;
    }
    if (body->len + (size_t)evt->data_len >= body->cap) {
        ESP_LOGW(TAG, "response longer than %u bytes, truncating",
                 (unsigned)body->cap);
        return ESP_OK;
    }

    memcpy(body->buf + body->len, evt->data, (size_t)evt->data_len);
    body->len += (size_t)evt->data_len;
    body->buf[body->len] = '\0';
    return ESP_OK;
}

/* One POST, JSON in (or none), the parsed JSON out. The caller owns the
 * cJSON. NULL means the exchange itself failed -- no answer, or one that is
 * not JSON -- which is a different thing from an answer carrying an error
 * code, and the two are handled differently by both callers. */
static cJSON *post(const char *path, const char *json, int timeout_ms)
{
    char url[192];

    snprintf(url, sizeof(url), "%s%s", CONFIG_APP_SERVER_URL, path);

    body_t body = { .buf = malloc(BODY_MAX), .cap = BODY_MAX };

    if (body.buf == NULL) {
        return NULL;
    }
    body.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = timeout_ms,
        .event_handler     = on_http_event,
        .user_data         = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    cJSON                   *root = NULL;

    if (http == NULL) {
        free(body.buf);
        return NULL;
    }

    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_post_field(http, json != NULL ? json : "",
                                   json != NULL ? (int)strlen(json) : 0);

    esp_err_t err    = esp_http_client_perform(http);
    int       status = esp_http_client_get_status_code(http);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s: %s", path, esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGW(TAG, "POST %s: HTTP %d", path, status);
    } else {
        root = cJSON_Parse(body.buf);
        if (root == NULL) {
            ESP_LOGW(TAG, "POST %s: not JSON (%u bytes)", path, (unsigned)body.len);
        }
    }

    esp_http_client_cleanup(http);
    free(body.buf);
    return root;
}

static int code_of(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItem(root, "code");

    return cJSON_IsNumber(code) ? code->valueint : -1;
}

static const char *string_in(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(object, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* ------------------------------------------------------------- the session */

/* POST /pc_session. `flag` 1 says this client understands quick login, which
 * is what the phone's confirm step sets; `userId` is deliberately absent --
 * that field is for a desktop client switching accounts with a cookie still
 * in hand, and a board has neither.
 *
 * The platform matters more here than anywhere else in this file: the app
 * server mints the IM token for the platform recorded on the session, and
 * WFC_PLATFORM is what the client will present at /route. Send anything else
 * and the token that comes back authenticates as something this board is
 * not (wfc_platform.h). */
static bool create_session(char *out, size_t out_size)
{
    char body[256];

    snprintf(body, sizeof(body),
             "{\"flag\":1,\"platform\":%d,\"device_name\":\"%s\",\"clientId\":\"%s\"}",
             WFC_PLATFORM, WFC_DEVICE_NAME, app_cfg_client_id());

    cJSON *root = post("/pc_session", body, 15000);

    if (root == NULL) {
        return false;
    }

    bool        ok    = false;
    int         code  = code_of(root);
    const char *token = string_in(cJSON_GetObjectItem(root, "result"), "token");

    if (code == 0 && token != NULL && token[0] != '\0') {
        strlcpy(out, token, out_size);
        ok = true;
    } else {
        ESP_LOGW(TAG, "/pc_session answered %d", code);
    }

    cJSON_Delete(root);
    return ok;
}

typedef enum {
    POLL_AGAIN = 0,   /* nothing happened yet */
    POLL_SCANNED,     /* a phone has it; waiting for the tap on 确认 */
    POLL_DONE,
    POLL_GONE,        /* cancelled, expired, or refused: start a new session */
    POLL_ERROR,       /* the exchange failed; the session may still be good */
} poll_result_t;

/* POST /session_login/<token>. Hangs for up to 50 seconds server-side and
 * answers the moment the session changes state.
 *
 * Code 8 is the ambiguous one: the server sends it both for "this session is
 * gone" and as the long poll's own timeout. They are told apart by how long
 * the request took -- a timeout takes the full 50 seconds, a dead session
 * comes back at once -- which is a heuristic, and the cost of getting it
 * wrong either way is one extra QR code. */
static poll_result_t poll_session(const char *token, char *user_id, size_t id_size,
                                  char *im_token, size_t token_size, char *who,
                                  size_t who_size)
{
    char path[APP_LOGIN_SESSION_MAX + 32];

    snprintf(path, sizeof(path), "/session_login/%s", token);

    int64_t started = esp_timer_get_time() / 1000;
    cJSON  *root    = post(path, NULL, POLL_TIMEOUT_MS);
    int64_t took    = esp_timer_get_time() / 1000 - started;

    if (root == NULL) {
        return POLL_ERROR;
    }

    poll_result_t  result = POLL_ERROR;
    int            code   = code_of(root);
    const cJSON   *inner  = cJSON_GetObjectItem(root, "result");

    switch (code) {
    case 0: {
        const char *uid = string_in(inner, "userId");
        const char *tok = string_in(inner, "token");

        if (uid != NULL && tok != NULL) {
            strlcpy(user_id, uid, id_size);
            strlcpy(im_token, tok, token_size);
            result = POLL_DONE;
        } else {
            ESP_LOGE(TAG, "a confirmed login with no account in it");
            result = POLL_GONE;
        }
        break;
    }
    case 9: {
        const char *name = string_in(inner, "userName");

        if (name != NULL) {
            strlcpy(who, name, who_size);
        }
        result = POLL_SCANNED;
        break;
    }
    case 10:
        result = POLL_AGAIN;
        break;
    case 18:
        ESP_LOGI(TAG, "the phone cancelled");
        result = POLL_GONE;
        break;
    case 8:
        result = took > 30000 ? POLL_AGAIN : POLL_GONE;
        break;
    default:
        ESP_LOGW(TAG, "/session_login answered %d", code);
        result = POLL_GONE;
        break;
    }

    cJSON_Delete(root);
    return result;
}

/* ------------------------------------------------------------------ public */

esp_err_t app_login_scan(void)
{
    const int64_t give_up = esp_timer_get_time() / 1000 + GIVE_UP_MS;

    ui_login_begin("");
    ui_goto(UI_PAGE_LOGIN);
    ui_log(UI_LOG_NOTE, "等待扫码登录");

    while (esp_timer_get_time() / 1000 < give_up) {
        char session[APP_LOGIN_SESSION_MAX];

        if (!create_session(session, sizeof(session))) {
            ui_login_phase(UI_LOGIN_FAIL, "取不到二维码，重试中");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char qr[128];

        snprintf(qr, sizeof(qr), "wildfirechat://pcsession/%s", session);
        ESP_LOGI(TAG, "login session %s", session);
        ui_login_begin(qr);

        const int64_t expires = esp_timer_get_time() / 1000 + SESSION_LIFE_MS;

        while (esp_timer_get_time() / 1000 < expires) {
            char user_id[APP_CFG_ID_MAX]    = "";
            char im_token[APP_CFG_TOKEN_MAX] = "";
            char who[64]                    = "";

            switch (poll_session(session, user_id, sizeof(user_id), im_token,
                                 sizeof(im_token), who, sizeof(who))) {
            case POLL_DONE: {
                esp_err_t err = app_cfg_set_account(user_id, im_token);

                if (err != ESP_OK) {
                    /* Storing it is the whole point: an account that is not
                     * in NVS is one the next boot will ask for again. */
                    ESP_LOGE(TAG, "cannot store the account: %s",
                             esp_err_to_name(err));
                    ui_login_phase(UI_LOGIN_FAIL, "存不下这个账号");
                    return err;
                }
                ESP_LOGI(TAG, "logged in as %s", user_id);
                ui_login_phase(UI_LOGIN_OK, user_id);
                ui_logf(UI_LOG_NOTE, "已登录 %s", user_id);
                return ESP_OK;
            }
            case POLL_SCANNED:
                ui_login_phase(UI_LOGIN_SCANNED, who);
                break;
            case POLL_AGAIN:
                break;
            case POLL_GONE:
                goto next_session;
            case POLL_ERROR:
            default:
                ui_login_phase(UI_LOGIN_FAIL, "服务器没有响应，重试中");
                vTaskDelay(pdMS_TO_TICKS(3000));
                break;
            }
        }

    next_session:
        /* Either the code on the screen aged out or the phone cancelled.
         * Both mean the same thing here: draw a new one. */
        ui_login_phase(UI_LOGIN_WAIT, "二维码已刷新");
    }

    ESP_LOGE(TAG, "nobody scanned the login code");
    return ESP_ERR_TIMEOUT;
}
