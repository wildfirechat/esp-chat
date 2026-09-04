#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "app_cfg.h"
#include "app_net.h"
#include "ui.h"
#include "ui_page.h"
#include "ui_setup.h"

static const char *TAG = "app_net";

/* How long the 配网 screen stays up before the board gives up and the caller
 * restarts it. Long enough to survive "I will do it after dinner", short
 * enough that a board left in a drawer is not an open access point forever. */
#define PROVISION_TIMEOUT_MS (60 * 60 * 1000)

/* One candidate network: three association attempts, and a ceiling on the
 * whole thing in case the driver reports neither success nor failure. */
#define STA_ATTEMPTS         3
#define CANDIDATE_TIMEOUT_MS 30000

/* Long enough for the phone's status poll to come round once more and see
 * that it worked, before the access point it is polling over goes away. */
#define GOODBYE_MS           4000

#define SCAN_MAX_APS         24
#define SCAN_FRESH_MS        30000

#define BIT_GOT_IP    BIT0
#define BIT_STA_FAIL  BIT1
#define BIT_CANDIDATE BIT2

/* Not `mode_t`: that is POSIX's, and sys/types.h is two includes away. */
typedef enum {
    MODE_IDLE = 0,
    MODE_PROVISION,
    MODE_RUN,
} net_mode_t;

static EventGroupHandle_t s_events;
static SemaphoreHandle_t  s_lock;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static httpd_handle_t     s_httpd;
static bool               s_wifi_started;

static volatile net_mode_t s_mode;
/* Whether a candidate association is in flight, which is what tells the
 * disconnect handler the difference between "try again" and "the caller asked
 * for this disconnect". */
static volatile bool s_trying;
static volatile int  s_attempts;

static ui_prov_phase_t s_phase;

/* --------------------------------------------------- the shared state
 *
 * Written by the boot path and the WiFi events, read by the HTTP handlers on
 * the server's own task. Small and fixed-size, so one mutex over the lot is
 * cheaper than being clever.
 */

static char s_cand_ssid[APP_CFG_SSID_MAX];
static char s_cand_pass[APP_CFG_PASS_MAX];

static const char *s_state = "idle";   /* what GET /wifi/status reports */
static char        s_state_ssid[APP_CFG_SSID_MAX];
static char        s_ip[16];
static int         s_reason;           /* the driver's disconnect reason */

static struct {
    char    ssid[APP_CFG_SSID_MAX];
    int8_t  rssi;
    uint8_t auth;
} s_aps[SCAN_MAX_APS];
static size_t  s_ap_count;
static int64_t s_scan_ms;

static char s_ap_ssid[APP_CFG_SSID_MAX];
static char s_ap_pass[16];

static bool lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void set_state(const char *state, const char *ssid, int reason)
{
    if (!lock()) {
        return;
    }
    s_state  = state;
    s_reason = reason;
    if (ssid != NULL) {
        strlcpy(s_state_ssid, ssid, sizeof(s_state_ssid));
    }
    xSemaphoreGive(s_lock);
}

static void set_phase(ui_prov_phase_t phase, const char *detail)
{
    s_phase = phase;
    ui_provision_phase(phase, detail);
}

/* The handful of disconnect reasons a person can act on. Everything else is
 * reported as a number, which is still better than "failed": the numbers are
 * in esp_wifi_types.h and searching for one finds the answer. */
static const char *reason_text(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "没找到这个网络";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "密码不对";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "加密方式对不上";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "信号太弱";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_LEAVE:
        return "网络断开了";
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------------ events */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* In provisioning the station side is idle until a phone posts
         * something; connecting here would dial the last network the driver
         * happened to have in its config. */
        if (s_mode == MODE_RUN) {
            ui_set_link_state("连接 WiFi", false);
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;

        xEventGroupClearBits(s_events, BIT_GOT_IP);
        ui_set_wifi(NULL, 0);
        ui_set_ip(NULL);

        if (s_mode == MODE_RUN) {
            ESP_LOGW(TAG, "WiFi disconnected (%d), retrying", e->reason);
            ui_set_link_state("WiFi 断开，重连中", false);
            esp_wifi_connect();
        } else if (s_trying) {
            /* A wrong passphrase looks exactly like a weak signal at the
             * first attempt, so a couple of tries come before the verdict. */
            if (++s_attempts < STA_ATTEMPTS) {
                ESP_LOGW(TAG, "candidate failed (%d), attempt %d", e->reason,
                         s_attempts + 1);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "candidate gave up (%d)", e->reason);
                set_state("failed", NULL, e->reason);
                xEventGroupSetBits(s_events, BIT_STA_FAIL);
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_mode == MODE_PROVISION && s_phase == UI_PROV_WAIT) {
            set_phase(UI_PROV_PHONE, NULL);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_mode == MODE_PROVISION && s_phase == UI_PROV_PHONE) {
            set_phase(UI_PROV_WAIT, NULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        char                     ip[16];

        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got ip %s", ip);

        if (lock()) {
            strlcpy(s_ip, ip, sizeof(s_ip));
            xSemaphoreGive(s_lock);
        }
        ui_set_ip(ip);
        ui_set_link_state("WiFi 已连接", false);
        app_net_report();
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

/* -------------------------------------------------------------------- STA */

/* The authentication floor, not the authentication mode: it is the weakest
 * access point this will associate with. WPA rather than WPA2 because a home
 * router in mixed mode still advertises the older one, and refusing to
 * associate with the network the user just picked from our own scan list is a
 * worse failure than joining it over WPA. An empty passphrase means the user
 * picked an open network and meant it. */
static void sta_config(const char *ssid, const char *password)
{
    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = (password == NULL || password[0] == '\0')
                                      ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };

    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password != NULL ? password : "",
            sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

void app_net_report(void)
{
    wifi_ap_record_t ap;

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ui_set_wifi((const char *)ap.ssid, ap.rssi);
    } else {
        ui_set_wifi(NULL, 0);
    }
}

/* ------------------------------------------------------------------ scan */

/* Scanning takes the radio off the access point's channel for a few seconds,
 * which a phone sitting on that access point experiences as the board going
 * away mid-request. So this only ever runs while nobody is associated, and
 * what the phone reads is a cached list -- with its age, so it can say
 * "刚刚" or offer a manual entry if the list is old and wrong.
 *
 * The list being the *board's* view rather than the phone's is the point of
 * having it at all: this radio is 2.4 GHz only, so a 5 GHz network the phone
 * can see is one the board cannot join, and the classic provisioning failure
 * is a person typing the right password for the wrong band. */
static void scan_refresh(void)
{
    wifi_scan_config_t scan = { .show_hidden = false };
    uint16_t           found = SCAN_MAX_APS;
    wifi_ap_record_t  *recs;

    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        return;
    }

    recs = calloc(SCAN_MAX_APS, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return;
    }
    if (esp_wifi_scan_get_ap_records(&found, recs) != ESP_OK) {
        free(recs);
        return;
    }

    if (!lock()) {
        free(recs);
        return;
    }

    s_ap_count = 0;
    for (uint16_t i = 0; i < found && s_ap_count < SCAN_MAX_APS; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        bool        seen = false;

        if (ssid[0] == '\0') {
            continue;
        }
        /* The same network reaches us through more than one access point in
         * any office; the phone wants a list of networks, not of radios. The
         * records arrive strongest first, so the first one wins. */
        for (size_t j = 0; j < s_ap_count; j++) {
            if (strcmp(s_aps[j].ssid, ssid) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        strlcpy(s_aps[s_ap_count].ssid, ssid, sizeof(s_aps[s_ap_count].ssid));
        s_aps[s_ap_count].rssi = recs[i].rssi;
        s_aps[s_ap_count].auth = (uint8_t)recs[i].authmode;
        s_ap_count++;
    }
    s_scan_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_lock);

    free(recs);
    ESP_LOGI(TAG, "scan found %u networks (%u after merging)", (unsigned)found,
             (unsigned)s_ap_count);
}

static bool phone_is_on_the_ap(void)
{
    wifi_sta_list_t stations = { 0 };

    return esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0;
}

/* ------------------------------------------------------------- HTTP server
 *
 * Three endpoints, all JSON, all on 192.168.4.1. The phone side of them is
 * DeviceProvisionActivity in the Android app.
 */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);

    cJSON_free(body);
    return err;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *aps  = cJSON_AddArrayToObject(root, "aps");

    if (root == NULL || aps == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }

    cJSON_AddNumberToObject(root, "code", 0);

    if (lock()) {
        for (size_t i = 0; i < s_ap_count; i++) {
            cJSON *ap = cJSON_CreateObject();

            if (ap == NULL) {
                break;
            }
            cJSON_AddStringToObject(ap, "ssid", s_aps[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", s_aps[i].rssi);
            cJSON_AddNumberToObject(ap, "auth", s_aps[i].auth);
            cJSON_AddItemToArray(aps, ap);
        }
        cJSON_AddNumberToObject(root, "age",
                                s_scan_ms > 0
                                    ? (esp_timer_get_time() / 1000 - s_scan_ms) / 1000
                                    : -1);
        xSemaphoreGive(s_lock);
    }

    return send_json(req, root);
}

static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddNumberToObject(root, "code", 0);
    if (lock()) {
        cJSON_AddStringToObject(root, "state", s_state);
        cJSON_AddStringToObject(root, "ssid", s_state_ssid);
        cJSON_AddStringToObject(root, "ip", s_ip);
        cJSON_AddNumberToObject(root, "reason", s_reason);
        xSemaphoreGive(s_lock);
    }
    /* So the phone can name the board it is talking to, and so that the whole
     * exchange can be checked from a laptop with curl. */
    cJSON_AddStringToObject(root, "device", app_cfg_client_id());
    return send_json(req, root);
}

static esp_err_t config_post(httpd_req_t *req)
{
    /* An SSID is at most 32 bytes and a passphrase 63, so anything much
     * bigger than this is not a provisioning request. */
    char   body[256];
    size_t len = req->content_len;

    if (len == 0 || len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    size_t got = 0;

    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);

        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' ||
        strlen(ssid->valuestring) >= APP_CFG_SSID_MAX ||
        (pass != NULL && cJSON_IsString(pass) &&
         strlen(pass->valuestring) >= APP_CFG_PASS_MAX)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ssid or password");
        return ESP_FAIL;
    }

    if (lock()) {
        strlcpy(s_cand_ssid, ssid->valuestring, sizeof(s_cand_ssid));
        strlcpy(s_cand_pass,
                cJSON_IsString(pass) ? pass->valuestring : "",
                sizeof(s_cand_pass));
        s_state  = "connecting";
        s_reason = 0;
        strlcpy(s_state_ssid, s_cand_ssid, sizeof(s_state_ssid));
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "a phone posted a network to try");

    /* Answer first, act second: the association that follows moves the radio
     * to the candidate's channel, and the phone is on this one. */
    cJSON *out = cJSON_CreateObject();

    cJSON_AddNumberToObject(out, "code", 0);
    esp_err_t err = send_json(req, out);

    xEventGroupSetBits(s_events, BIT_CANDIDATE);
    return err;
}

static void httpd_up(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();

    /* cJSON on the handler's stack, and a scan list to serialise. */
    cfg.stack_size      = 6144;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "the provisioning HTTP server would not start");
        return;
    }

    static const httpd_uri_t URIS[] = {
        { .uri = "/wifi/scan",   .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/wifi/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/wifi/config", .method = HTTP_POST, .handler = config_post },
    };

    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        httpd_register_uri_handler(s_httpd, &URIS[i]);
    }
}

static void httpd_down(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

/* -------------------------------------------------------------- the AP */

/* Named after the board and not after the deployment: two of these on one
 * desk have to be told apart, and the last three bytes of the MAC are what
 * the label on the back would say if it had one. The passphrase is new on
 * every provisioning run -- it is only ever read off this screen, so there is
 * nothing to be gained by it outliving the exchange. */
static void ap_identity(void)
{
    uint8_t mac[6] = { 0 };

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "WFChat-%02X%02X%02X", mac[3], mac[4],
             mac[5]);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08" PRIx32, esp_random());
}

static void ap_up(void)
{
    wifi_config_t cfg = {
        .ap = {
            .channel        = 1,
            .max_connection = 2,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = { .required = false },
        },
    };

    strlcpy((char *)cfg.ap.ssid, s_ap_ssid, sizeof(cfg.ap.ssid));
    strlcpy((char *)cfg.ap.password, s_ap_pass, sizeof(cfg.ap.password));
    cfg.ap.ssid_len = strlen(s_ap_ssid);

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* APSTA, not AP: the station half is what tries the network the phone
     * posts, and it has to be up while the phone is still watching. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }
}

static void ap_down(void)
{
    httpd_down();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
}

/* ------------------------------------------------------------------ public */

void app_net_init(void)
{
    s_events = xEventGroupCreate();
    s_lock   = xSemaphoreCreateMutex();
    assert(s_events != NULL && s_lock != NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
}

esp_err_t app_net_provision(void)
{
    char qr[128];

    s_mode = MODE_PROVISION;
    ap_identity();
    ap_up();

    /* Before anyone is associated, which is the only time it is free. */
    scan_refresh();
    httpd_up();

    /* The same shape the phone already knows from every other WildFireChat QR
     * code: a scheme, a kind, a value, and the rest in the query string
     * (WfcScheme.java). The value is the access point to join and `pwd` is
     * how to join it. */
    snprintf(qr, sizeof(qr), "wildfirechat://espwifi/%s?pwd=%s", s_ap_ssid,
             s_ap_pass);

    ESP_LOGI(TAG, "provisioning: %s", qr);
    ui_logf(UI_LOG_NOTE, "配网中：热点 %s", s_ap_ssid);
    ui_provision_begin(qr, s_ap_ssid, s_ap_pass);
    ui_goto(UI_PAGE_PROVISION);

    const int64_t deadline = esp_timer_get_time() / 1000 + PROVISION_TIMEOUT_MS;

    while (esp_timer_get_time() / 1000 < deadline) {
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CANDIDATE, pdTRUE,
                                               pdFALSE, pdMS_TO_TICKS(10000));

        if ((bits & BIT_CANDIDATE) == 0) {
            /* Nobody yet. Keep the list the phone is about to ask for from
             * going stale, but only while asking is free. */
            int64_t age = 0;

            if (lock()) {
                age = esp_timer_get_time() / 1000 - s_scan_ms;
                xSemaphoreGive(s_lock);
            }
            if (age > SCAN_FRESH_MS && !phone_is_on_the_ap()) {
                scan_refresh();
            }
            continue;
        }

        char ssid[APP_CFG_SSID_MAX];
        char pass[APP_CFG_PASS_MAX];

        if (!lock()) {
            continue;
        }
        strlcpy(ssid, s_cand_ssid, sizeof(ssid));
        strlcpy(pass, s_cand_pass, sizeof(pass));
        xSemaphoreGive(s_lock);

        set_phase(UI_PROV_TRYING, ssid);
        ui_logf(UI_LOG_NOTE, "试连 %s", ssid);

        xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_STA_FAIL);
        s_attempts = 0;
        s_trying   = true;
        sta_config(ssid, pass);
        esp_wifi_connect();

        bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_STA_FAIL, pdFALSE,
                                   pdFALSE, pdMS_TO_TICKS(CANDIDATE_TIMEOUT_MS));
        s_trying = false;

        if (bits & BIT_GOT_IP) {
            esp_err_t err = app_cfg_set_wifi(ssid, pass);

            if (err != ESP_OK) {
                /* The board is on the network but will not remember it. Say
                 * so rather than pretending: the next boot would be back
                 * here with no explanation. */
                ESP_LOGE(TAG, "cannot store the network: %s", esp_err_to_name(err));
                ui_log(UI_LOG_ERROR, "配网成功，但没能存下来");
            }

            set_state("connected", ssid, 0);
            set_phase(UI_PROV_OK, ssid);
            ui_logf(UI_LOG_NOTE, "已连上 %s", ssid);

            /* The phone is still polling over the access point this is about
             * to take down. */
            vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));

            /* Before ap_down() and not after: dropping the AP half moves the
             * radio, and if that costs us the association we have just made,
             * the handler should be the one that reconnects rather than
             * leaving the caller to notice. */
            s_mode = MODE_RUN;
            ap_down();
            return ESP_OK;
        }

        /* Neither an address nor a verdict means the driver said nothing for
         * half a minute, which is a failure with no reason code. */
        int reason = 0;

        if (lock()) {
            reason = (bits & BIT_STA_FAIL) ? s_reason : 0;
            xSemaphoreGive(s_lock);
        }
        if ((bits & BIT_STA_FAIL) == 0) {
            set_state("failed", ssid, 0);
        }
        esp_wifi_disconnect();

        const char *why = reason_text(reason);
        char        detail[64];

        if (why != NULL) {
            snprintf(detail, sizeof(detail), "%s：%s", ssid, why);
        } else {
            snprintf(detail, sizeof(detail), "%s（%d）", ssid, reason);
        }
        set_phase(UI_PROV_FAIL, detail);
        ui_logf(UI_LOG_ERROR, "配网失败 %s", detail);
    }

    ESP_LOGE(TAG, "nobody finished provisioning");
    ap_down();
    return ESP_ERR_TIMEOUT;
}

esp_err_t app_net_start(void)
{
    s_mode = MODE_RUN;

    /* Provisioning leaves the board associated, so there is nothing to do but
     * say which mode we are in now. */
    if (xEventGroupGetBits(s_events) & BIT_GOT_IP) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    sta_config(app_cfg_wifi_ssid(), app_cfg_wifi_password());

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        /* Started already, so WIFI_EVENT_STA_START will not come round
         * again to do this for us. */
        esp_wifi_connect();
    }

    xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}
