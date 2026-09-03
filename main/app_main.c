/* Bring-up: WiFi, a real clock, and then one call into each of the two SDKs.
 *
 * P4 moved the protocol behind wfc_client.h and left this file doing the
 * drawing: it subscribed to every event, read the store, resolved every name
 * and pushed finished strings at the panel. P5 moves that too. A chat page
 * cannot be fed from here -- opening a conversation has to read its messages,
 * and the application does not know when someone opens one -- so the UI reads
 * the store itself, through the same public API any client would use.
 *
 * What is left is what an application actually is: bring the network up, hand
 * over credentials, and tell the screen the three things the client cannot
 * find out on its own -- the WiFi, the address, the account.
 *
 * Success looks like: tapping a conversation opens it, typing into it sends,
 * and the reply appears without anything here knowing it happened -- and a
 * phone calling the board makes it ring, without anything here knowing that
 * either.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wfc_client.h"

#include "custom_message.h"
#include "ui.h"

static const char *TAG = "app";

#define NET_GOT_IP_BIT BIT0

/* A Kconfig bool that is n does not appear in sdkconfig.h at all, so give it a
 * value here instead of threading #ifdef through the code. */
#ifdef CONFIG_WFC_PULL_HISTORY
#define PULL_HISTORY 1
#else
#define PULL_HISTORY 0
#endif

static EventGroupHandle_t s_net_events;

/* ------------------------------------------------------------------- WiFi */

static void report_wifi(void)
{
    wifi_ap_record_t ap;

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ui_set_wifi((const char *)ap.ssid, ap.rssi);
    } else {
        ui_set_wifi(NULL, 0);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ui_set_link_state("连接 WiFi", false);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_net_events, NET_GOT_IP_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        ui_set_link_state("WiFi 断开，重连中", false);
        ui_set_wifi(NULL, 0);
        ui_set_ip(NULL);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        char               ip[16];

        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got ip %s", ip);
        ui_set_ip(ip);
        ui_set_link_state("WiFi 已连接", false);
        report_wifi();
        xEventGroupSetBits(s_net_events, NET_GOT_IP_BIT);
    }
}

static void wifi_start(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_APP_WIFI_SSID,
            .password = CONFIG_APP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ------------------------------------------------------------------- time */

/* Risk R4 in ASSESSMENT.md: every encrypted payload carries an "hours since
 * 2018-01-01 00:00 UTC+8" prefix and the server validates it, so an unsynced
 * clock does not degrade the connection -- it prevents it. mqtt_box falls back
 * to uptime; here that would only turn a clear failure into a confusing one,
 * so this blocks until the clock is real. */
static bool sync_clock(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_APP_SNTP_SERVER);

    setenv("TZ", CONFIG_APP_TIMEZONE, 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    ui_set_link_state("对时中", false);

    for (int attempt = 1; attempt <= 6; attempt++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
            time_t    now = time(NULL);
            struct tm tm;
            char      stamp[32];

            localtime_r(&now, &tm);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
            ESP_LOGI(TAG, "clock synced: %s", stamp);
            ui_logf(UI_LOG_NOTE, "对时完成 %s", stamp);
            return true;
        }
        ESP_LOGW(TAG, "SNTP attempt %d timed out", attempt);
        ui_logf(UI_LOG_NOTE, "SNTP 第 %d 次超时", attempt);
    }

    ESP_LOGE(TAG, "no time sync from %s -- the IM server rejects requests with a "
                  "stale timestamp, so there is nothing useful to do without it",
             CONFIG_APP_SNTP_SERVER);
    ui_set_link_state("对时失败", false);
    ui_log(UI_LOG_ERROR, "对时失败，无法连接服务器");
    return false;
}

/* ---------------------------------------------------------------- identity */

/* Decision D2: default to a stable MAC-derived client ID, overridable from
 * config. The override is what you want with a test token, since a token is
 * bound to the client ID it was minted for. */
static const char *client_id(void)
{
    static char derived[32];

    if (strlen(CONFIG_WFC_CLIENT_ID) > 0) {
        return CONFIG_WFC_CLIENT_ID;
    }

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(derived, sizeof(derived), "esp32s3-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGW(TAG, "no client ID configured, derived %s from the MAC -- a token "
                  "minted for another client ID will not authenticate",
             derived);
    return derived;
}

static bool config_is_complete(void)
{
    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "no WiFi SSID; see README (sdkconfig.local)");
        ui_log(UI_LOG_ERROR, "未配置 WiFi，见 README");
        return false;
    }
    if (strlen(CONFIG_WFC_USER_ID) == 0 || strlen(CONFIG_WFC_TOKEN) == 0) {
        ESP_LOGE(TAG, "no WFC user ID / token; see README (sdkconfig.local)");
        ui_log(UI_LOG_ERROR, "未配置账号 / token，见 README");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------- main */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Screen first, so a configuration mistake shows up on the panel instead
     * of only on a serial console nobody has attached. */
    ui_init();
    ui_log(UI_LOG_NOTE, "启动中");

    if (!config_is_complete()) {
        return;
    }

    wfc_client_config_t cfg = {
        .host         = CONFIG_WFC_HOST,
        .route_port   = CONFIG_WFC_ROUTE_PORT,
        .user_id      = CONFIG_WFC_USER_ID,
        .client_id    = client_id(),
        .token        = CONFIG_WFC_TOKEN,
        .pull_history = PULL_HISTORY,
    };

    /* Opens the store, which is why it comes before the network: the panel
     * then fills from the last run rather than from the first packet.
     *
     * A store that will not open is fatal on purpose. Carrying on would keep
     * messages flowing but silently drop both halves of what P3 is for: the
     * head would stop persisting, and deduplication -- which is "ask the
     * store" -- would stop working, so every pushed message would show up
     * twice. */
    err = wfc_client_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the client did not start: %s -- if this is the store, check "
                      "that partitions.csv has the 'storage' partition, or build "
                      "with CONFIG_WFC_STORE_RAM",
                 esp_err_to_name(err));
        ui_set_link_state("存储打开失败", false);
        ui_logf(UI_LOG_ERROR, "存储打开失败: %s", esp_err_to_name(err));
        return;
    }

    /* This deployment's own message types, before anything can arrive: the
     * table decides how a message is stored and what its conversation row
     * says, and that is computed once, as the message goes in. A type
     * registered after its messages are already filed does not go back and
     * fix them (wfc_content.h). */
    custom_messages_register();

    ui_set_account(CONFIG_WFC_USER_ID);
    ui_client_ready();

    /* Calls before the link comes up, not after: the engine subscribes to the
     * message stream, and an invite that arrives in the first seconds of a
     * connection is exactly the one a board that was just switched on is
     * likely to get. Nothing here is fatal and nothing here touches audio
     * hardware; in a build with CONFIG_APP_CALL=n it is one empty function
     * and the AV SDK is not linked at all (ui_page.h). */
    ui_call_start();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_net_events = xEventGroupCreate();

    wifi_start();
    xEventGroupWaitBits(s_net_events, NET_GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    if (!sync_clock()) {
        return;
    }

    /* The one blocking call: route, connect, authenticate. Everything after
     * it reaches the screen as an event. */
    ui_set_link_state("请求 route", false);
    if (wfc_client_connect() != ESP_OK) {
        ui_logf(UI_LOG_ERROR, "连接失败: %s", wfc_status_str(wfc_client_status()));
        return;
    }

    /* Signal strength is the one status row that has to be polled -- there is
     * no event for "the WiFi got weaker". */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        report_wifi();
    }
}
