/* How a custom message looks on the panel.
 *
 * MessageContentContainerView.vue's half of the job: the chat page draws a
 * bubble for anything it does not recognise, and this table decides which
 * types it does not recognise. What a view IS -- one function, on the UI task
 * with the display lock held, drawing from the envelope and nothing else --
 * is ../ui_msg_view.h, and it is worth reading before adding one.
 *
 * Most types want no entry here:
 *
 *   different WORDS   change the type's digest in custom_message_config.c.
 *                     The conversation list and the bubble read the same one,
 *                     so they stay in step for free.
 *   a centred notice  register the type with .notification = true. The page
 *                     draws it exactly like the built-in group notices.
 *   nothing special   put a readable line in searchable_content and you get a
 *                     bubble, a conversation row and an unread count.
 *
 * What is left for this file is types that have to LOOK different.
 */

#include <stddef.h>

#include "ui_page.h"

#include "custom_message.h"

/* -------------------------------------------------------------- the view */

/* The test type's bubble: the ordinary one with a label over it, which is the
 * cheapest thing that is visibly not a text message. A real custom view -- a
 * work order with a button, a reading with a gauge -- is built the same way,
 * from this container and these colours.
 *
 * Note what it does NOT do: go looking for its body. That arrived in
 * searchable_content, so row->text already holds it by the time this runs --
 * put the body where every WFC client looks for it and the only thing left to
 * write is the appearance. A type whose body will not fit in a line of text
 * reads it back out of the store from the chat page's prime(), keyed by
 * row->message_uid; it does not get carried along in the row. */
static void draw_test(lv_obj_t *parent, const ui_msg_row_t *row)
{
    lv_obj_t *line = ui_msg_line(parent, row);

    /* The tag stands in for what a real view puts here -- a title, an icon,
     * the sender in a group (row->who, when row->group). */
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

/* ------------------------------------------------------------- the table */

static const ui_msg_view_t CUSTOM_VIEWS[] = {
    {
        .type = MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST,
        .draw = draw_test,
    },
    /* MESSAGE_CONTENT_TYPE_CUSTOM_MESSAGE_TEST_NOTIFICATION is deliberately
     * absent. It is registered as a notification, so the page draws it as a
     * centred line exactly like the built-in group notices, and its digest
     * already reads the tip out of MessageContent.content. A type that wants
     * to look like the rest of the client should say so in the type table
     * rather than draw itself here. */
};

const ui_msg_view_t *custom_message_view(int32_t type)
{
    for (size_t i = 0; i < sizeof(CUSTOM_VIEWS) / sizeof(CUSTOM_VIEWS[0]); i++) {
        if (CUSTOM_VIEWS[i].type == type) {
            return &CUSTOM_VIEWS[i];
        }
    }
    return NULL;
}
