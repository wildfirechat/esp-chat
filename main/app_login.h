/* 扫码登录 -- how a board with no account gets one.
 *
 * The board has no keyboard and no camera, so the only credential path that
 * does not end in "type a 200-byte token into a serial console" is the one
 * every desktop WildFireChat client already uses: the board asks the app
 * server for a login session, draws the session as a QR code, and a phone
 * that is already logged in scans it and confirms. The app server then mints
 * an IM token for THIS board -- its client ID, its platform -- and hands it
 * back over the long poll the board is already sitting in.
 *
 * The point of doing it exactly this way is that the phone needs no changes
 * at all: wildfirechat://pcsession/<token> is what a desktop client shows,
 * WfcScheme.java already routes it, and PCLoginActivity already has the
 * "允许登录" sheet. To the app server this board is a slightly unusual PC.
 *
 * Four endpoints are involved and this file only calls two of them; the other
 * two are the phone's half:
 *
 *   POST /pc_session          board  -> a session token, valid 5 minutes
 *   POST /scan_pc/<token>     phone  -> marks it scanned
 *   POST /confirm_pc          phone  -> marks it confirmed
 *   POST /session_login/<t>   board  -> long poll: 0 done, 9 scanned,
 *                                       10 not yet, 18 cancelled, 8 gone
 *
 * The token that comes back is bound to the client ID sent when the session
 * was created, which is why app_cfg.h stores the client ID rather than
 * deriving it each boot: a board that changes its mind about who it is has
 * thrown its account away.
 *
 * Needs the network up AND the clock right -- the app server is https and a
 * certificate is checked against the date.
 */

#ifndef APP_LOGIN_H
#define APP_LOGIN_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws the 扫码登录 page and blocks until someone confirms a login on their
 * phone, at which point the account is in NVS (app_cfg.h) and the caller can
 * start the client. ESP_ERR_TIMEOUT if nobody does within about an hour --
 * the caller's cue to restart rather than to keep a stale QR code on a screen
 * for a week.
 *
 * Runs on the boot path. Blocks for up to a minute at a time inside the long
 * poll, which is the intended shape: the screen belongs to another task. */
esp_err_t app_login_scan(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOGIN_H */
