/* What the pages and the shell agree on.
 *
 * The panel is five pages behind one header: three that are "home" and reached
 * from the nav bar, and two that are pushed on top of them and reached by
 * tapping something.
 *
 *   会话  the conversation list          home
 *   状态  what the board is doing        home
 *   日志  the message log                home
 *   chat  one conversation               pushed from 会话
 *   写    the composer                   pushed from chat
 *
 * A page is four functions and no state that outlives its widgets. create()
 * builds the tree, refresh() fills it from the store, destroy() forgets the
 * pointers, title() names it in the header. Leaving a page deletes every
 * widget in it, so nothing a page holds may be read after destroy() -- which
 * is why anything that has to survive (the log's backlog, the status fields)
 * lives in a plain array beside the widgets rather than in them.
 *
 * That is the same shape as the rest of the client: the store is the model,
 * the screen is a projection of it, and a redraw is a re-read rather than a
 * patch. It is what makes "a profile arrived, redraw the names" one line
 * instead of a search for the labels that happen to be showing an ID.
 *
 * ------------------------------------------------------------------------
 * Threading, and the one rule that matters.
 *
 * create(), refresh(), destroy() and title() run on the UI task WITH THE
 * DISPLAY LOCK HELD, so none of them may block. Reading the store is fine --
 * it takes its own mutex and returns. Anything that can reach the network is
 * not: wfc_get_user_info() and wfc_get_group_info() send a UPUI or a GPGI on
 * a cache miss, wfc_send_text() sends an MS, and all three end up in a
 * blocking send() on the long link's socket (wfc_mqtt.c). Doing that while
 * holding the display lock stops LVGL from repainting AND from reading the
 * touch panel, which does not look like a slow refresh -- it looks like the
 * board has died, with the back button no longer working.
 *
 * So fetching and sending go in prime(), which the UI task calls after
 * releasing the lock. prime() may block; it may not touch a widget.
 *
 * The corollary for LVGL event callbacks -- a button, a row -- is the same
 * rule from the other side: they already run on the LVGL task with the lock
 * held, so they may only post (ui_goto, ui_back, ui_compose_open) and must
 * never call into the client.
 *
 * A page may also read the store from inside another store callback -- the
 * mutex is recursive -- but must not fetch there, for a second reason: the
 * client takes its own lock on a cache miss, and taking it while holding the
 * store's is the one lock order the client forbids (ASSESSMENT.md 8.6).
 * Collect IDs during the walk, ask in prime().
 */

#ifndef UI_PAGE_H
#define UI_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#include "ui.h"
#include "wfc_client.h"

/* ------------------------------------------------------------------ theme */

/* Dark, because the panel sits on a desk and is looked at sideways, and
 * because a white 320x240 IPS at full backlight is unpleasant in a room. */
#define UI_C_BG      0x0E1116
#define UI_C_PANEL   0x161B22
#define UI_C_RAISED  0x21262D   /* a bubble, a chip, a pressed row */
#define UI_C_LINE    0x30363D
#define UI_C_ACCENT  0x2F81F7   /* our own messages, the send button */
#define UI_C_TEXT    0xE6EDF3
#define UI_C_DIM     0x8B949E
#define UI_C_OK      0x3FB950
#define UI_C_BAD     0xF85149
#define UI_C_IN      0x7EE787

/* One CJK size, because a second one is another 480 KB of flash (see
 * components/wfc_font/). Small Latin text -- timestamps, counters -- can use
 * Montserrat 12, which LVGL builds in and which never has to draw a
 * Chinese character. */
#define UI_FONT_SMALL (&lv_font_montserrat_12)

/* The longest header title, which is also the longest conversation name a
 * page will show. */
#define UI_TITLE_MAX 48

/* ------------------------------------------------------------------ redraw */

/* What changed. Set from any task; the UI task collects them and repaints at
 * most once per tick, which is what keeps a catch-up delivering forty
 * messages from repainting the conversation list forty times. */
#define UI_DIRTY_CONVS    (1u << 0)   /* the list, or a row in it */
#define UI_DIRTY_MESSAGES (1u << 1)   /* messages arrived or were sent */
#define UI_DIRTY_NAMES    (1u << 2)   /* a profile landed: anything showing a name */
#define UI_DIRTY_STATUS   (1u << 3)
#define UI_DIRTY_LOG      (1u << 4)
#define UI_DIRTY_CALL     (1u << 5)   /* a call started, changed state or ended */
#define UI_DIRTY_ALL      0xFFFFFFFFu

void ui_dirty(uint32_t bits);

/* -------------------------------------------------------------- navigation */

typedef enum {
    UI_PAGE_CONVS = 0,
    UI_PAGE_STATUS,
    UI_PAGE_LOG,
    UI_PAGE_CHAT,
    UI_PAGE_COMPOSE,
    UI_PAGE_CALL,
    UI_PAGE_COUNT,
} ui_page_id_t;

typedef struct {
    void (*create)(lv_obj_t *parent);
    void (*refresh)(uint32_t dirty);
    /* Called after the display lock is released, so this is where anything
     * that can block goes: asking the server for a missing profile, sending
     * a message. Optional. Must not touch widgets. */
    void (*prime)(void);
    void (*destroy)(void);
    /* Written into `buf`; always writes something. */
    void (*title)(char *buf, size_t buf_size);
    /* Home pages sit under the nav bar and have no back arrow. */
    bool home;
} ui_page_def_t;

/* Both are queue posts: safe from a button callback on the LVGL task, safe
 * from an event callback on the wfc task. The switch happens on the UI task a
 * tick later. */
void ui_goto(ui_page_id_t page);
void ui_back(void);

/* Which page is showing. On the UI task only. */
ui_page_id_t ui_current_page(void);

/* ------------------------------------------------------------- the pages */

extern const ui_page_def_t ui_page_convs;
extern const ui_page_def_t ui_page_chat;
extern const ui_page_def_t ui_page_compose;
extern const ui_page_def_t ui_page_status;
extern const ui_page_def_t ui_page_log;
extern const ui_page_def_t ui_page_call;

/* ui_convs.c -> ui_chat.c. Sets the conversation and navigates to it. */
void ui_chat_open(const wfc_conversation_t *conv);

/* ui_chat.c -> ui_compose.c. The composer is deliberately not told what a
 * conversation is: it collects text and hands it back. Voice, when it comes,
 * is another page with this same signature and nothing else changes. */
typedef void (*ui_compose_cb_t)(const char *text);
void ui_compose_open(const char *title, ui_compose_cb_t on_send);

/* ------------------------------------------------------------ the calls
 *
 * Everything the application knows about voice calls is behind the handful of
 * functions below, plus ui_call_start() in ui.h, and they are the reason no
 * file here except ui_call.c includes wfav.h.
 *
 * Calls are the one feature that can be left out of a build: the AV SDK is a
 * separate module (../wfav-esp) and CONFIG_APP_CALL=n compiles ui_call_none.c
 * in place of ui_call.c. That is the same trick the client plays with its two
 * store backends -- one header, two implementations, exactly one compiled --
 * and it has the same payoff: the shell and the chat page are written once,
 * with no #ifdef in them, and the build with no AV SDK is not a second version
 * of them that nobody looks at.
 *
 * The stub answers false to ui_call_available() and does nothing else. So the
 * only caller that has to ask is the chat page, which would otherwise draw a
 * phone button that cannot do anything; the rest just call and get silence. */

/* Whether this build can call at all. A compile-time fact, reached through a
 * function so that the callers do not have to know that. */
bool ui_call_available(void);

/* Subscribe the page to the AV SDK's events. Called once from ui_init(),
 * alongside the client's own subscriptions and for the same reasons. */
void ui_call_subscribe(void);

/* ui_call_start(), which brings the AV engine up, is the one of these the
 * application calls rather than the screen, so it is declared in ui.h with
 * everything else app_main.c uses. */

/* Is a call up? The chat page asks so that its phone button can mean "back to
 * the call" instead of "place one". */
bool ui_call_busy(void);

/* ui_chat.c -> ui_call.c. Place a call to `target`, or, if one is already up,
 * just show it again. Safe from an LVGL button callback: starting a call sends
 * an IM message and then talks to a TURN server, and this returns before
 * either happens. */
void ui_call_dial(const char *target);

/* ui.c -> ui_call.c, and ui_chat.c -> ui_call.c. Unlike every other page,
 * this one is opened by something the user did not do: an invite arrives and
 * the board has to ring. So the call page reads the session itself
 * (wfav_session.h) rather than being handed anything, exactly as the other
 * pages read the store.
 *
 * Safe from any task; it is a queue post like ui_goto(). Opening it while it
 * is already up does nothing, which is what makes "an invite arrived" and
 * "the user tapped 通话" the same call. */
void ui_call_open(void);

/* ui_call.c -> ui.c -> ui_call.c, the long way round on purpose.
 *
 * The phrase a call ended with -- "对方已挂断", "未接听" -- comes from
 * the event, because by the time the page repaints the session is IDLE and
 * IDLE does not say why. But the event arrives on the AV task and the page
 * belongs to the UI task, so the string goes through the shell's queue:
 * ui_call_post_end_reason() posts it from wherever, and the UI task hands it
 * back to ui_call_set_end_reason(). */
void ui_call_post_end_reason(const char *why);   /* any task; ui.c */
void ui_call_set_end_reason(const char *text);   /* UI task; ui_call.c */

/* ui.c -> ui_status.c, ui_log.c: the facts the app pushes in. */
void ui_status_set_wifi(const char *ssid, int rssi);
void ui_status_set_ip(const char *ip);
void ui_status_set_account(const char *user_id);
void ui_log_append(ui_log_kind_t kind, const char *line);

/* ------------------------------------------------------------- helpers */

/* A container with our background, no border, no padding, no scrollbar --
 * LVGL's default object is a white rounded card, and every one of these
 * would otherwise need six lines to undo that. */
void ui_style_flat(lv_obj_t *obj);

/* A flat container that lays its children out in a column, full width. */
lv_obj_t *ui_column(lv_obj_t *parent, int32_t pad);

/* A label that wraps at the parent's width. */
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, uint32_t color);

/* "14:03", or "昨天 14:03", or "3月5日" -- how a conversation row and a
 * message stamp say when, from a WFC millisecond timestamp. */
void ui_format_time(int64_t ms, char *buf, size_t buf_size);

#endif /* UI_PAGE_H */
