/* The network: the WiFi this board is on, and how it is told which one.
 *
 * Everything about associating with an access point used to be twenty lines
 * in app_main.c reading two Kconfig strings. It is a file now because there
 * are two ways in rather than one:
 *
 *   configured   the stored SSID and password (app_cfg.h), which is every
 *                boot after the first
 *   配网         no stored network, so the board becomes an access point of
 *                its own, shows a QR code naming it, and waits for a phone
 *                to join and post one
 *
 * The second one is why this is a state machine and not a function. While it
 * runs, the radio is in APSTA: the board is serving its own network to the
 * phone AND trying the candidate the phone posted, which is what lets the
 * phone be told "that password was wrong" instead of losing the board the
 * moment it tries. The access point is only taken down once the station side
 * has an address and the phone has had a moment to read that it did.
 *
 * Why an access point and a plain HTTP endpoint rather than the ESP-IDF
 * provisioning manager (protocomm + protobuf over SoftAP or BLE):
 *
 *   - BLE is what the phone-side UX would prefer, and it does not fit: the
 *     NimBLE stack plus protocomm is several hundred KB of flash and ota_0
 *     has about 500 KB left with calls compiled in (README.md).
 *   - protocomm's SoftAP transport would fit, but its security handshake
 *     needs the espressif provisioning library on the phone, and the phone
 *     here is our own chat app rather than a provisioning tool. Three JSON
 *     endpoints are less code on both sides than a protobuf schema and a
 *     curve25519 handshake -- and the link they run over is already WPA2 with
 *     a passphrase that only exists on the board's screen.
 *
 * ------------------------------------------------------------------------
 * Threading. app_net_provision() and app_net_start() block and run on the
 * boot path (the main task). The HTTP handlers run on the server's own task
 * and touch nothing here except through one mutex; the WiFi events run on the
 * system event task and only set bits and push strings at the screen.
 */

#ifndef APP_NET_H
#define APP_NET_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* netif, the default event loop, the WiFi driver and this file's handlers.
 * Call once, before either of the two below. */
void app_net_init(void);

/* Become an access point and wait for a phone to hand over a network that
 * works. Returns once the board is associated and has an address, with the
 * credentials already stored (app_cfg.h) -- or ESP_ERR_TIMEOUT if nobody
 * finished the exchange within about an hour, which is the caller's cue to
 * restart rather than to sit on a dead screen forever.
 *
 * Draws its own screen: the 配网 page, through ui_provision.h. */
esp_err_t app_net_provision(void);

/* Associate with the stored network and block until there is an address.
 * Returns immediately if provisioning just did that. From here on a dropped
 * link is retried forever, which is what the board on a shelf needs. */
esp_err_t app_net_start(void);

/* Push the current SSID and signal strength at the status page. There is no
 * event for "the WiFi got weaker", so the boot path polls this. */
void app_net_report(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_NET_H */
