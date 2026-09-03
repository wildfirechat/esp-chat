/* How a custom message looks on the panel.
 *
 * MessageContentContainerView.vue's job: the chat page draws a bubble for
 * anything it does not recognise, and this decides which types it does not
 * recognise. Two seams, because a message is drawn in two moments that are on
 * different tasks and have different things available:
 *
 *   custom_message_text()  runs inside the store query, where the message and
 *                          its payload are still there. Anything that has to
 *                          be decoded is decoded here.
 *   custom_message_draw()  runs on the UI task with the display lock, where
 *                          there is a widget tree and no message.
 *
 * A type that only wants different words implements the first. A type that
 * wants to look different implements the second. A type that follows the
 * convention -- a readable line in searchable_content -- implements neither
 * and still gets a bubble, a conversation row and an unread count, which is
 * the case worth optimising for.
 */

#include <stdio.h>
#include <string.h>

#include "ui_page.h"

#include "custom_message.h"

/* -------------------------------------------------------------- the text */

bool custom_message_text(const wfc_message_t *msg, char *buf, size_t buf_size)
{
    switch (msg->content.type) {
    case MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST:
        /* The bubble says more than the conversation row does: the row has
         * one line for the whole conversation and shows the body alone, while
         * here there is room to say what kind of message it is. Decoding a
         * JSON payload -- the usual shape of a real custom message -- belongs
         * exactly here, where content.data is still valid. */
        snprintf(buf, buf_size, "%s", msg->content.searchable_content);
        return true;

    case MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST_NOTIFICATION:
        /* Registered as a notification, so the page centres it; the text is
         * the tip, which travels in MessageContent.content. */
        snprintf(buf, buf_size, "%s", msg->content.content);
        return true;

    default:
        return false;
    }
}

/* -------------------------------------------------------------- the view */

/* The test type's bubble: the ordinary one with a label over it, which is the
 * cheapest thing that is visibly not a text message. A real custom view --
 * a work order with a button, a reading with a gauge -- is built the same
 * way, from this parent and these colours. */
static void draw_test(lv_obj_t *parent, const custom_message_row_t *row)
{
    lv_obj_t *line = lv_obj_create(parent);

    ui_style_flat(line);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(line, 1, 0);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START,
                          row->mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          row->mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);

    /* The tag stands in for what a real view puts here -- a title, an icon,
     * the sender in a group (row->who). */
    lv_obj_t *tag = lv_label_create(line);

    lv_obj_set_style_text_font(tag, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(tag, lv_color_hex(UI_C_IN), 0);
    lv_label_set_text(tag, "CUSTOM 1001");

    lv_obj_t *bubble = lv_label_create(line);

    lv_label_set_long_mode(bubble, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(bubble, LV_PCT(78), 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 7, 0);
    /* A left border in the accent colour: enough to read as "not a chat
     * message" at arm's length, which is all a demonstration owes. */
    lv_obj_set_style_border_side(bubble, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(UI_C_ACCENT), 0);
    lv_obj_set_style_border_width(bubble, 3, 0);
    lv_obj_set_style_text_color(bubble, lv_color_hex(UI_C_TEXT), 0);
    lv_label_set_text(bubble, row->text);
}

bool custom_message_draw(lv_obj_t *parent, const custom_message_row_t *row)
{
    switch (row->type) {
    case MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST:
        draw_test(parent, row);
        return true;

    default:
        /* Including the notification: it is registered as one, so the page's
         * own centred line already draws it -- the same way the built-in
         * group notices are drawn, which is the point of registering it that
         * way instead of drawing it here. */
        return false;
    }
}
