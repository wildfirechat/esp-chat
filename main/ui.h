/* The screen: what the application hands to it, and nothing else.
 *
 * Up to P4 this header was a drawing API -- the app read the store, resolved
 * every name, formatted every row and pushed finished strings at a panel that
 * knew nothing. P5 turns that around, because a chat page cannot work that
 * way: opening a conversation has to load its messages, and leaving it has to
 * drop them, and neither is something the application can push in advance.
 *
 * So the UI reads the store itself, through wfc_client.h, exactly as any
 * other client would. What is left here is the handful of facts the UI cannot
 * find out on its own: the WiFi it is on, the address it was given, the
 * account it was configured with, and the stages of a startup that happen
 * before there is a client to ask.
 *
 * ------------------------------------------------------------------------
 * Threading, which is the same contract as before. Every function here is a
 * queue post: any task may call any of them, including the wfc task from
 * inside an event callback. Nothing here blocks and nothing here draws --
 * the drawing happens later, on the UI task, under the display lock.
 *
 * The UI subscribes to the client's events itself (ui.c), so there is no
 * event plumbing left in the application.
 */

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a logged line is about. It used to pick the colour of a row on the
 * 日志 page; now it picks a mark and a level on the console (ui.c). */
typedef enum {
    UI_LOG_NOTE = 0,   /* something happened */
    UI_LOG_IN,         /* a message arrived */
    UI_LOG_OUT,        /* a message went out */
    UI_LOG_ERROR,
} ui_log_kind_t;

/* Brings up display, touch, the theme and the widget tree, and subscribes to
 * the client's events. Call once, first -- before wfc_client_init(), so a
 * configuration mistake lands on the panel instead of only on a serial
 * console nobody has attached. */
void ui_init(void);

/* The store is open. Everything drawn until now was drawn against a closed
 * store, which is to say empty; this is the cue to draw it again with the
 * previous run's conversations in it. Call once, after wfc_client_init(). */
void ui_client_ready(void);

/* Bring voice calls up: start the AV engine behind the call page. Call once,
 * after wfc_client_init() and before wfc_client_connect() -- the engine
 * listens on the message stream, and the invite a board is most likely to get
 * is the one already waiting when it switches on.
 *
 * Never fatal, and never blocking on hardware: a board that came up as a chat
 * client is more useful than one that refused to boot, so a failure is logged
 * and the phone button stays dark, and no audio device is touched until there
 * is a call. In a build with CONFIG_APP_CALL=n this is an empty function and
 * the AV SDK is not linked at all -- see ui_page.h for how that is arranged.
 */
void ui_call_start(void);

/* Bring push-to-talk up: register its content types and start listening.
 * Call once, after wfc_client_init() and before wfc_client_connect(), for
 * both of the reasons ui_call_start() gives -- a content type registered
 * after its messages have been filed does not fix the rows that were filed
 * without it, and the first thing a board that was just switched on is likely
 * to get is somebody already talking.
 *
 * Never fatal and never blocking on hardware: no codec is touched until there
 * is audio. In a build with CONFIG_APP_PTT=n this is an empty function and the
 * PTT SDK is not linked at all (ui_page.h). */
void ui_ptt_start(void);

/* The line in the header, for the stages that happen before the client exists
 * -- WiFi, SNTP, /route. Once the long link is up the UI takes this over from
 * the connection-status event and the application stops calling it. */
void ui_set_link_state(const char *text, bool ok);

/* `ssid` NULL means not connected; `rssi` is dBm. */
void ui_set_wifi(const char *ssid, int rssi);
void ui_set_ip(const char *ip);          /* NULL clears it */
void ui_set_account(const char *user_id);

/* A line about what the board just did, in the language the panel is in.
 * There is no longer a page showing these -- 我的 took the 日志 tab -- so they
 * go to the serial console, at ESP_LOGW for UI_LOG_ERROR and ESP_LOGI for the
 * rest. Safe from any task. */
void ui_log(ui_log_kind_t kind, const char *text);
void ui_logf(ui_log_kind_t kind, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
