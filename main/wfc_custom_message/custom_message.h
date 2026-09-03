/* Custom messages: what the rest of the application needs to know.
 *
 * Three functions, and one of them is the seam the chat page calls without
 * knowing what is behind it. Adding a message type touches this directory and
 * nothing else -- that is the point of the directory, and it is why the
 * client component has a type table instead of a switch statement
 * (wfc_content.h), and why the screen has one too (../ui_msg_view.h).
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

#include "ui_msg_view.h"

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

/* -------------------------------------------------------- drawing (seam) */

/* This deployment's view for `type`, or NULL when it has none and the chat
 * page should draw its ordinary bubble.
 *
 * One function rather than the two the chat page used to call, because what a
 * view is now lives in ../ui_msg_view.h: one draw(), on the UI task with the
 * display lock held, drawing from the envelope the page hands it. A type that
 * needs more than the envelope reads it back out of the store by
 * row->message_uid, from prime(); nothing type-specific rides along in the
 * row. That header is where the rule and the reasons are written down.
 *
 * Looked up BEFORE the built-in views, so registering type 3 here replaces
 * the picture view the same way registering type 3 in the type table replaces
 * its digest. */
const ui_msg_view_t *custom_message_view(int32_t type);

#endif /* CUSTOM_MESSAGE_H */
