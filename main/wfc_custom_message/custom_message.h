/* Custom messages: what the rest of the application needs to know.
 *
 * Four functions, and three of them are seams the chat page calls without
 * knowing what is behind them. Adding a message type touches this directory
 * and nothing else -- that is the point of the directory, and it is why the
 * client component has a type table instead of a switch statement
 * (wfc_content.h).
 *
 * The web client's src/wfc_custom_message/ is the same five steps in the same
 * order; README.md here maps one onto the other.
 */

#ifndef CUSTOM_MESSAGE_H
#define CUSTOM_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#include "esp_err.h"

#include "wfc_client.h"

#include "custom_message_type.h"

/* Registers every type in custom_message_config.c with the client. Call once,
 * after wfc_client_init() and before wfc_client_connect(): the table is read
 * as messages are stored, and the first ones can arrive on the CONNACK. */
void custom_messages_register(void);

/* --------------------------------------------------------------- sending */

/* The two examples, sent into `conv`. Both are ordinary wfc_send_message()
 * calls -- there is no custom-message send path -- and both are stored,
 * listed and redrawn like a text message because their registration says
 * they persist. */
esp_err_t custom_message_send_test(const wfc_conversation_t *conv, const char *text);
esp_err_t custom_message_send_test_notification(const wfc_conversation_t *conv,
                                                const char *tip);

/* ------------------------------------------------------- drawing (seams) */

/* The line of text a chat bubble shows for this message. False means "no
 * opinion" and the page falls back to wfc_message_digest(), which is what
 * every registered type without its own view gets.
 *
 * Called while the store's message is still valid (inside the query
 * callback), so this is where a payload gets decoded -- afterwards there is
 * only the copy in the row. */
bool custom_message_text(const wfc_message_t *msg, char *buf, size_t buf_size);

/* One message as the chat page has it by drawing time: the payload is long
 * gone (it died with the store callback), so what a view gets is what
 * custom_message_text() put in `text` plus the envelope. A view that needs
 * more than this needs custom_message_text() to have kept it. */
typedef struct {
    int32_t     type;
    const char *text;
    const char *who;        /* the sender's display name, "" for our own */
    int64_t     timestamp;
    bool        mine;
} custom_message_row_t;

/* Draws the row into `parent`. False means "no opinion" and the page draws
 * its ordinary bubble.
 *
 * Runs on the UI task with the display lock held: build widgets, read
 * nothing that can block, call nothing in wfc_client.h. */
bool custom_message_draw(lv_obj_t *parent, const custom_message_row_t *row);

#endif /* CUSTOM_MESSAGE_H */
