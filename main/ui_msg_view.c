/* The table: which type is drawn by which view.
 *
 * MessageContentContainerView.vue's job, and the whole of it -- one lookup
 * and one call. The views themselves are one file each (ui_msg_image.c,
 * wfc_custom_message/custom_message_view.c), the same way the web client has
 * one .vue per content type.
 */

#include <stddef.h>

#include "ui_page.h"
#include "ui_msg_view.h"

#include "custom_message.h"

/* WFC's own types that need more than a bubble. Anything not here -- text,
 * the group notices, a type nobody has taught this client about -- is drawn
 * by the page, which is the right answer for all three. */
static const ui_msg_view_t *const BUILTIN_VIEWS[] = {
    &ui_msg_view_image,
};

bool ui_msg_view_draw(lv_obj_t *parent, const ui_msg_row_t *row)
{
    /* The deployment's own views first. A registration replaces a built-in
     * type in the client's table (wfc_content.h); this is the same rule for
     * the same reason, so a fork can put its own picture view in without
     * editing this file. */
    const ui_msg_view_t *view = custom_message_view(row->type);

    if (view == NULL) {
        for (size_t i = 0; i < sizeof(BUILTIN_VIEWS) / sizeof(BUILTIN_VIEWS[0]); i++) {
            if (BUILTIN_VIEWS[i]->type == row->type) {
                view = BUILTIN_VIEWS[i];
                break;
            }
        }
    }

    if (view == NULL) {
        return false;
    }
    view->draw(parent, row);
    return true;
}

/* --------------------------------------------------------------- helper */

lv_obj_t *ui_msg_line(lv_obj_t *parent, const ui_msg_row_t *row)
{
    lv_obj_t *line = lv_obj_create(parent);

    ui_style_flat(line);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(line, 1, 0);
    lv_obj_set_flex_align(line,
                          LV_FLEX_ALIGN_START,
                          row->mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          row->mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
    return line;
}
