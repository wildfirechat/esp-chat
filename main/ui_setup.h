/* The two pages a board shows before it is a chat client: 配网 and 扫码登录.
 *
 * They are the odd ones out on this panel. Every other page is a projection
 * of the store (ui_page.h) and reads what it draws; these two draw something
 * that does not exist anywhere except in the exchange that is running -- a QR
 * code and how far it has got -- so they are pushed at, like the log is.
 *
 * The push comes from app_net.c and app_login.c, on the boot path or on the
 * HTTP server's task, and lands in a small static under a mutex that the UI
 * task reads when it repaints. That is ui_log.c's arrangement and it is here
 * for the same reason: the writer must never block on the display lock.
 *
 * Both pages are `setup` pages: no nav bar and no back arrow, because there
 * is nowhere else to be until the exchange finishes. The board leaves them by
 * navigating away when it does.
 */

#ifndef UI_SETUP_H
#define UI_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 配网 */

typedef enum {
    UI_PROV_WAIT = 0,   /* the QR is up; nobody has joined the access point */
    UI_PROV_PHONE,      /* a phone is on it */
    UI_PROV_TRYING,     /* a network was posted and is being tried */
    UI_PROV_OK,
    UI_PROV_FAIL,       /* that one did not work; the QR is still up */
} ui_prov_phase_t;

/* The QR payload, and the two things it encodes, written out for anyone who
 * would rather type them into the phone's WiFi settings by hand. */
void ui_provision_begin(const char *qr, const char *ap_ssid, const char *ap_password);
void ui_provision_phase(ui_prov_phase_t phase, const char *detail);

/* ------------------------------------------------------------ 扫码登录 */

typedef enum {
    UI_LOGIN_WAIT = 0,  /* the QR is up and the long poll is out */
    UI_LOGIN_SCANNED,   /* a phone scanned it; waiting for the tap on 确认 */
    UI_LOGIN_OK,
    UI_LOGIN_FAIL,      /* the exchange failed; a new QR is on its way */
} ui_login_phase_t;

void ui_login_begin(const char *qr);
void ui_login_phase(ui_login_phase_t phase, const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* UI_SETUP_H */
