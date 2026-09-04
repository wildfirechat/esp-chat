#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_cfg.h"

static const char *TAG = "app_cfg";

/* One namespace, five keys. NVS key names are capped at 15 characters, which
 * is why the WiFi pair is not spelled out. */
#define NS          "wfc"
#define K_SSID      "wifi_ssid"
#define K_PASS      "wifi_pass"
#define K_USER      "user_id"
#define K_CLIENT    "client_id"
#define K_TOKEN     "token"

typedef struct {
    char wifi_ssid[APP_CFG_SSID_MAX];
    char wifi_password[APP_CFG_PASS_MAX];
    char user_id[APP_CFG_ID_MAX];
    char client_id[APP_CFG_ID_MAX];
    char token[APP_CFG_TOKEN_MAX];
} app_cfg_t;

static app_cfg_t s_cfg;

/* ------------------------------------------------------------------ NVS */

static void read_str(nvs_handle_t nvs, const char *key, char *out, size_t size)
{
    size_t len = size;

    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static esp_err_t write_str(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open NVS to write %s: %s", key, esp_err_to_name(err));
        return err;
    }

    /* An empty string is stored as "no key at all", so that "is this board
     * configured" is one question and not two. */
    if (value == NULL || value[0] == '\0') {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(nvs, key, value);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot write %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

/* -------------------------------------------------------------- identity */

/* Decision D2: a stable ID derived from the WiFi MAC. Only ever called on a
 * boot that found no stored client ID -- see the header for why this is not
 * recomputed every time even though it would come out the same. */
static void derive_client_id(char *out, size_t size)
{
    uint8_t mac[6] = { 0 };

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, size, "esp32s3-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------ load */

/* Kconfig is the seed, not the source. The distinction only shows up on a
 * board that has been configured once and then reflashed with a different
 * sdkconfig.local: the stored values win, and the build's are ignored. That
 * is the right way round -- a token scanned onto the board yesterday should
 * survive a firmware update today -- but it is surprising enough to say out
 * loud, because the alternative reading ("I changed sdkconfig.local and
 * nothing happened") is exactly what it looks like from the outside. */
static void seed(nvs_handle_t nvs, const char *key, char *field, size_t size,
                 const char *from_kconfig, const char *what)
{
    if (field[0] != '\0') {
        if (from_kconfig[0] != '\0' && strcmp(field, from_kconfig) != 0) {
            ESP_LOGW(TAG, "%s comes from NVS, not from sdkconfig -- the build's "
                          "value is ignored until it is cleared on the device",
                     what);
        }
        return;
    }
    if (from_kconfig[0] == '\0') {
        return;
    }

    strlcpy(field, from_kconfig, size);
    if (nvs_set_str(nvs, key, field) == ESP_OK) {
        ESP_LOGI(TAG, "%s seeded from sdkconfig", what);
    }
}

void app_cfg_load(void)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &nvs);

    if (err != ESP_OK) {
        /* Not fatal, and not silent: the board comes up as an unconfigured
         * one, which is a screen asking for a phone rather than a stop. */
        ESP_LOGE(TAG, "NVS would not open (%s) -- treating this board as "
                      "unconfigured", esp_err_to_name(err));
        memset(&s_cfg, 0, sizeof(s_cfg));
        derive_client_id(s_cfg.client_id, sizeof(s_cfg.client_id));
        return;
    }

    read_str(nvs, K_SSID, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    read_str(nvs, K_PASS, s_cfg.wifi_password, sizeof(s_cfg.wifi_password));
    read_str(nvs, K_USER, s_cfg.user_id, sizeof(s_cfg.user_id));
    read_str(nvs, K_CLIENT, s_cfg.client_id, sizeof(s_cfg.client_id));
    read_str(nvs, K_TOKEN, s_cfg.token, sizeof(s_cfg.token));

    seed(nvs, K_SSID, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid),
         CONFIG_APP_WIFI_SSID, "WiFi SSID");
    seed(nvs, K_PASS, s_cfg.wifi_password, sizeof(s_cfg.wifi_password),
         CONFIG_APP_WIFI_PASSWORD, "WiFi password");
    seed(nvs, K_USER, s_cfg.user_id, sizeof(s_cfg.user_id),
         CONFIG_WFC_USER_ID, "user ID");
    seed(nvs, K_TOKEN, s_cfg.token, sizeof(s_cfg.token),
         CONFIG_WFC_TOKEN, "token");

    /* The client ID is the one value that is never left empty: a board that
     * has not been told one still has to be able to ask for a token, and the
     * QR login sends this in the session it creates. */
    if (s_cfg.client_id[0] == '\0') {
        if (strlen(CONFIG_WFC_CLIENT_ID) > 0) {
            strlcpy(s_cfg.client_id, CONFIG_WFC_CLIENT_ID, sizeof(s_cfg.client_id));
        } else {
            derive_client_id(s_cfg.client_id, sizeof(s_cfg.client_id));
        }
        nvs_set_str(nvs, K_CLIENT, s_cfg.client_id);
        ESP_LOGI(TAG, "client ID for the life of this board: %s", s_cfg.client_id);
    }

    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "wifi %s, account %s, client %s",
             app_cfg_has_wifi() ? s_cfg.wifi_ssid : "(none)",
             app_cfg_has_account() ? s_cfg.user_id : "(none)", s_cfg.client_id);
}

/* ----------------------------------------------------------------- reads */

bool app_cfg_has_wifi(void)
{
    return s_cfg.wifi_ssid[0] != '\0';
}

bool app_cfg_has_account(void)
{
    return s_cfg.user_id[0] != '\0' && s_cfg.token[0] != '\0';
}

const char *app_cfg_wifi_ssid(void)     { return s_cfg.wifi_ssid; }
const char *app_cfg_wifi_password(void) { return s_cfg.wifi_password; }
const char *app_cfg_user_id(void)       { return s_cfg.user_id; }
const char *app_cfg_token(void)         { return s_cfg.token; }
const char *app_cfg_client_id(void)     { return s_cfg.client_id; }

/* ---------------------------------------------------------------- writes */

esp_err_t app_cfg_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
    strlcpy(s_cfg.wifi_password, password != NULL ? password : "",
            sizeof(s_cfg.wifi_password));

    esp_err_t err = write_str(K_SSID, s_cfg.wifi_ssid);

    if (err == ESP_OK) {
        err = write_str(K_PASS, s_cfg.wifi_password);
    }
    return err;
}

esp_err_t app_cfg_set_account(const char *user_id, const char *token)
{
    if (user_id == NULL || user_id[0] == '\0' || token == NULL || token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(token) >= APP_CFG_TOKEN_MAX) {
        ESP_LOGE(TAG, "token is %u bytes, the ceiling is %d -- it would be "
                      "stored truncated and fail at /route",
                 (unsigned)strlen(token), APP_CFG_TOKEN_MAX - 1);
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(s_cfg.user_id, user_id, sizeof(s_cfg.user_id));
    strlcpy(s_cfg.token, token, sizeof(s_cfg.token));

    esp_err_t err = write_str(K_USER, s_cfg.user_id);

    if (err == ESP_OK) {
        err = write_str(K_TOKEN, s_cfg.token);
    }
    return err;
}

esp_err_t app_cfg_clear_wifi(void)
{
    s_cfg.wifi_ssid[0]     = '\0';
    s_cfg.wifi_password[0] = '\0';

    esp_err_t err = write_str(K_SSID, NULL);

    if (err == ESP_OK) {
        err = write_str(K_PASS, NULL);
    }
    return err;
}

esp_err_t app_cfg_clear_account(void)
{
    s_cfg.user_id[0] = '\0';
    s_cfg.token[0]   = '\0';

    esp_err_t err = write_str(K_USER, NULL);

    if (err == ESP_OK) {
        err = write_str(K_TOKEN, NULL);
    }
    return err;
}
