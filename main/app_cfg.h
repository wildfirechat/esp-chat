/* What the board knows about itself between power cycles.
 *
 * Up to now this was three Kconfig strings compiled into the image: the WiFi
 * to associate with and the account to log in as (ASSESSMENT.md 5.5 and
 * decision D3, which both said "Kconfig 够用，直到开机配网 + 扫码登录"). This
 * is that move. The same four values now live in NVS, and the Kconfig ones
 * are what a blank NVS is seeded with -- so a bench board with a
 * sdkconfig.local still comes up exactly as it did, and a board without one
 * asks the phone instead of stopping.
 *
 * Four values, two pairs, and the pairs are independent on purpose:
 *
 *   wifi_ssid / wifi_password   set by 配网 (app_net.h), a phone on the
 *                               board's own access point
 *   user_id / token             set by 扫码登录 (app_login.h), which needs
 *                               the WiFi to already work
 *
 * The seeding happens on a store that is blank, not on one that is empty:
 * NVS also remembers that a pair was cleared on purpose, so a board that has
 * been told to forget its account does not come back up as whoever
 * sdkconfig.local names. Only erasing NVS puts the build's values back.
 *
 * plus client_id, which is neither: it is decided once, on the first boot,
 * and then never changes for the life of the board -- because the token is
 * minted FOR a client ID and a board that derives a new one has thrown its
 * account away. That is why it is stored rather than derived on each boot,
 * even though the derivation (from the WiFi MAC) is stable: storing it means
 * a Kconfig override that is later removed cannot silently invalidate a token
 * that was issued while it was there.
 *
 * ------------------------------------------------------------------------
 * Threading. Everything here is called from the boot path on the main task,
 * except that the setters may also be called from a page's button handler --
 * which is why each setter finishes by writing NVS rather than deferring, and
 * why the callers that clear something restart the board straight after. The
 * in-memory copy is not locked; nothing reads it from another task.
 */

#ifndef APP_CFG_H
#define APP_CFG_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 802.11 caps the SSID at 32 bytes and the WPA2 passphrase at 63; the token
 * is the app server's base64 blob, about 200 bytes on this deployment, and
 * the ceiling is generous because a longer one is a bad boot rather than a
 * truncated string nobody notices. */
#define APP_CFG_SSID_MAX   33
#define APP_CFG_PASS_MAX   64
#define APP_CFG_ID_MAX     64
#define APP_CFG_TOKEN_MAX  512

/* Reads NVS and, where NVS is empty, seeds it from Kconfig. Call once, before
 * anything asks whether the board is configured. Never fails in a way worth
 * handling: an NVS that will not open leaves the board unconfigured, which is
 * the same path as a board nobody has configured yet. */
void app_cfg_load(void);

bool app_cfg_has_wifi(void);
bool app_cfg_has_account(void);

const char *app_cfg_wifi_ssid(void);
const char *app_cfg_wifi_password(void);
const char *app_cfg_user_id(void);
const char *app_cfg_token(void);

/* Decision D2: derived from the WiFi MAC on the first boot, overridable from
 * Kconfig on that same first boot, and fixed afterwards. Never empty. */
const char *app_cfg_client_id(void);

/* Both commit to NVS before returning: the caller of the first one is a
 * provisioning exchange that is about to drop the access point it answered
 * on, and the caller of the second is a login that is about to connect. */
esp_err_t app_cfg_set_wifi(const char *ssid, const char *password);
esp_err_t app_cfg_set_account(const char *user_id, const char *token);

/* 重新配网 / 退出登录. Each clears its own pair and nothing else -- forgetting
 * an account is not a reason to forget the network it was reached over. The
 * caller restarts the board; there is no way to unwind a connected client.
 *
 * The clear survives that restart even on a board built with credentials in
 * sdkconfig.local: it is recorded as deliberate, and a deliberate empty is
 * not re-seeded. So 退出登录 really does land on the 扫码登录 QR code, and
 * the way back to the build's own account is `idf.py erase-flash`. */
esp_err_t app_cfg_clear_wifi(void);
esp_err_t app_cfg_clear_account(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CFG_H */
