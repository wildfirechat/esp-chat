/* Bring-up: a network, a real clock, an account, and then one call into each
 * of the two SDKs.
 *
 * P4 moved the protocol behind wfc_client.h and left this file doing the
 * drawing: it subscribed to every event, read the store, resolved every name
 * and pushed finished strings at the panel. P5 moved that too -- a chat page
 * cannot be fed from here, so the UI reads the store itself, through the same
 * public API any client would use.
 *
 * What is left is what an application actually is: decide what the board
 * knows about itself, get it onto a network and into an account, and hand
 * both to the client.
 *
 * The two "get it into" parts are what this file grew for. A board used to be
 * a board with credentials compiled into its image; now it is a board that
 * may have none, and there are two separate holes it may have to fill before
 * it can be a chat client:
 *
 *   no network   app_net_provision() -- become an access point, show a QR
 *                code, let a phone hand over a network (app_net.h)
 *   no account   app_login_scan() -- ask the app server for a login session,
 *                show it as a QR code, let a phone confirm it (app_login.h)
 *
 * Both write what they learn to NVS (app_cfg.h), so each of them happens
 * once in the life of a board rather than once per boot. A board that has
 * both -- every boot after the first, and every board configured through
 * sdkconfig.local -- takes exactly the path it took before this existed,
 * including the order: the store opens before the network so the panel comes
 * up with the last run's conversations on it rather than filling in after
 * the link does.
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
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "wfc_client.h"

#include "app_cfg.h"
#include "app_login.h"
#include "app_net.h"
#include "custom_message.h"
#include "ui.h"
#include "ui_page.h"

static const char *TAG = "app";

/* A Kconfig bool that is n does not appear in sdkconfig.h at all, so give it a
 * value here instead of threading #ifdef through the code. */
#ifdef CONFIG_WFC_PULL_HISTORY
#define PULL_HISTORY 1
#else
#define PULL_HISTORY 0
#endif

/* ------------------------------------------------------------------- time */

/* Risk R4 in ASSESSMENT.md: every encrypted payload carries an "hours since
 * 2018-01-01 00:00 UTC+8" prefix and the server validates it, so an unsynced
 * clock does not degrade the connection -- it prevents it. mqtt_box falls back
 * to uptime; here that would only turn a clear failure into a confusing one,
 * so this blocks until the clock is real. It is also what makes the app
 * server's certificate checkable, which the QR login needs a step later.
 *
 * "Blocks until" is meant literally, and that is a change: it used to give up
 * after six attempts, and app_main returned. That made an unsynced clock the
 * one failure on this path that needed a person -- the very thing the link
 * supervisor exists to avoid one step later (ASSESSMENT.md 8.11). It matters
 * because the minute after an access point comes back is exactly when DNS and
 * NTP are least likely to answer, so the old ceiling was most likely to be
 * hit in the case it most needed to survive: a board on a shelf at 3 a.m.
 *
 * Nothing here re-arms anything. lwip's SNTP client keeps sending on its own
 * retry schedule and resolves the server name afresh each time, so waiting
 * again is all this loop has to do. */
static void sync_clock(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_APP_SNTP_SERVER);

    setenv("TZ", CONFIG_APP_TIMEZONE, 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    ui_set_link_state("对时中", false);

    for (int attempt = 1;; attempt++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
            time_t    now = time(NULL);
            struct tm tm;
            char      stamp[32];

            localtime_r(&now, &tm);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
            ESP_LOGI(TAG, "clock synced: %s", stamp);
            ui_logf(UI_LOG_NOTE, "对时完成 %s", stamp);
            return;
        }

        /* The signal row is the only thing on the screen that can still change
         * while this loop runs, and a board stuck here is usually a board with
         * a bad link -- so it is worth keeping honest. */
        app_net_report();

        ESP_LOGW(TAG, "SNTP attempt %d timed out", attempt);

        /* Say it once at the point the old code gave up, then once a minute:
         * a board that waits out a ten-minute outage should not spend those
         * ten minutes writing the same line to the log. */
        if (attempt == 6) {
            ESP_LOGE(TAG, "no time sync from %s yet -- the IM server rejects requests "
                          "with a stale timestamp, so there is nothing to do but keep "
                          "asking", CONFIG_APP_SNTP_SERVER);
            ui_set_link_state("对时失败，重试中", false);
            ui_log(UI_LOG_ERROR, "对时失败，正在重试");
        } else if (attempt > 6 && attempt % 6 == 0) {
            ESP_LOGW(TAG, "still no time sync after %d attempts", attempt);
        }
    }
}

/* ----------------------------------------------------------- the client */

/* Everything that needs an account, in the order it needs to happen. Called
 * either before the network (the ordinary boot, where the account is already
 * in NVS and the panel should fill from the store while the WiFi comes up) or
 * straight after the QR login (the first boot, where there was nothing to
 * fill it with). */
static bool client_start(void)
{
    wfc_client_config_t cfg = {
        .route_port   = CONFIG_WFC_ROUTE_PORT,
        .user_id      = app_cfg_user_id(),
        .client_id    = app_cfg_client_id(),
        .token        = app_cfg_token(),
        .pull_history = PULL_HISTORY,
    };

    /* Opens the store, which is why this can come before the network: the
     * panel then fills from the last run rather than from the first packet.
     *
     * A store that will not open is fatal on purpose. Carrying on would keep
     * messages flowing but silently drop both halves of what P3 is for: the
     * head would stop persisting, and deduplication -- which is "ask the
     * store" -- would stop working, so every pushed message would show up
     * twice. */
    esp_err_t err = wfc_client_init(&cfg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the client did not start: %s -- if this is the store, check "
                      "that partitions.csv has the 'storage' partition, or build "
                      "with CONFIG_WFC_STORE_RAM",
                 esp_err_to_name(err));
        ui_set_link_state("存储打开失败", false);
        ui_logf(UI_LOG_ERROR, "存储打开失败: %s", esp_err_to_name(err));
        return false;
    }

    /* This deployment's own message types, before anything can arrive: the
     * table decides how a message is stored and what its conversation row
     * says, and that is computed once, as the message goes in. A type
     * registered after its messages are already filed does not go back and
     * fix them (wfc_content.h). */
    custom_messages_register();

    ui_set_account(app_cfg_user_id());
    ui_client_ready();

    /* Calls before the link comes up, not after: the engine subscribes to the
     * message stream, and an invite that arrives in the first seconds of a
     * connection is exactly the one a board that was just switched on is
     * likely to get. Nothing here is fatal and nothing here touches audio
     * hardware; in a build with CONFIG_APP_CALL=n it is one empty function
     * and the AV SDK is not linked at all (ui_page.h). */
    ui_call_start();

    /* Push-to-talk, for the same two reasons and with one more of its own:
     * its four content types have to be registered before anything can
     * arrive, or the conversation rows filed in the meantime keep a digest
     * computed without them (wfc_content.h). A separate switch from calls --
     * CONFIG_APP_PTT -- because a talk needs the microphone and the long
     * link and nothing else, so a build with no AV SDK can still be a
     * walkie-talkie. */
    ui_ptt_start();
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

    /* Screen first, so a board that is going to ask for a phone asks on the
     * panel rather than on a serial console nobody has attached. */
    ui_init();
    ui_log(UI_LOG_NOTE, "启动中");

    /* What this board knows about itself: the stored network and account, or
     * the sdkconfig.local values seeded into NVS on a first boot. */
    app_cfg_load();

    /* The ordinary boot: an account is known, so the store opens now and the
     * panel comes up with the previous run's conversations on it. */
    bool have_account = app_cfg_has_account();
    bool client_up    = have_account && client_start();

    if (have_account && !client_up) {
        return;   /* the store, and client_start() has already said so */
    }

    app_net_init();

    /* Hole one: no network. Becomes an access point and waits for a phone.
     * A board nobody ever comes back to restarts rather than sitting on a
     * stale QR code with an open access point up. */
    bool setup_shown = false;

    if (!app_cfg_has_wifi()) {
        setup_shown = true;
        if (app_net_provision() != ESP_OK) {
            ESP_LOGE(TAG, "provisioning timed out, restarting");
            esp_restart();
        }
    }

    app_net_start();
    sync_clock();

    /* Hole two: no account. Needs the network for the app server and the
     * clock for its certificate, which is why it is here and not up with the
     * store. */
    if (!client_up) {
        setup_shown = true;
        if (app_login_scan() != ESP_OK) {
            ESP_LOGE(TAG, "nobody logged the board in, restarting");
            esp_restart();
        }
        if (!client_start()) {
            return;
        }
    }

    /* The one blocking call: route, connect, authenticate. Everything after
     * it reaches the screen as an event.
     *
     * A failure here is not the end of the run any more. The client keeps
     * trying on its own, so the honest thing to do is say what happened and
     * carry on -- a board on a shelf whose access point came back at 3 a.m.
     * should be connected at 3 a.m., not waiting for someone to power-cycle
     * it. The exceptions are the answers that will not change (a token the
     * server refuses, being kicked off), and those stay on the status line
     * rather than being retried; the 状态 page has the button that clears the
     * account and asks for a new QR code. */
    /* Only when one of the two setup pages is what is on the screen: on an
     * ordinary boot the conversation list is already up, and switching to it
     * would tear it down and rebuild it for nothing. */
    if (setup_shown) {
        ui_goto(UI_PAGE_CONVS);
    }
    ui_set_link_state("请求 route", false);
    if (wfc_client_connect() != ESP_OK) {
        ui_logf(UI_LOG_ERROR, "连接失败: %s，将继续重试",
                wfc_status_str(wfc_client_status()));
    }

    /* Signal strength is the one status row that has to be polled -- there is
     * no event for "the WiFi got weaker". */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        app_net_report();
    }
}
