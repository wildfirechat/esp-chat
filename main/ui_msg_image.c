/* 图片消息 -- ImageMessageContentView.vue, at the stage this board can reach.
 *
 * The picture if there is one, and a frame the same size if there is not.
 * Which of the two it is, this file does not decide and does not wait for:
 * ui_media_get() answers straight away, and a NULL means "ask again next
 * repaint" rather than "no". Everything behind that -- the URL out of the
 * store, the download, the decode, the memory -- is ui_media.h, on a task of
 * its own, for reasons written down there.
 *
 * The frame is the same size as the picture will be, so nothing on the page
 * moves when it lands.
 *
 * ------------------------------------------------------------------------
 * One size for every picture, which is a decision rather than a stub.
 *
 * The sender's dimensions are on the wire ({"w":..,"h":..} in
 * MessageContent.content) and it is tempting to shape the frame with them.
 * The web client writes that object only WHEN IT KNOWS the size
 * (imageMessageContent.js: `if (this.imageWidth)`), so a good share of
 * messages carry no size at all -- a frame that fits some pictures and
 * quietly does not fit others is worse than one shape for all of them. The
 * decoded picture is then drawn inside the frame at its own shape, which is
 * the sender's claim replaced by the fact.
 *
 * ------------------------------------------------------------------------
 * What is on the wire (vue-chat/src/wfc/messages/imageMessageContent.js):
 *
 *   remoteMediaUrl   the full-size image on the file server. This is what is
 *                    fetched, read from the store by ui_media.c -- never
 *                    copied into a row, where a fixed field would clip the
 *                    signed URLs some deployments issue.
 *   binaryContent    a JPEG thumbnail, in MessageContent.data, and NOT used.
 *                    It would be the cheap path -- the web client compresses
 *                    it to under 7 KB and it is already local -- but the
 *                    store caps MessageContent.data at
 *                    CONFIG_WFC_STORE_MAX_DATA, 1024 bytes by default and
 *                    8192 at the Kconfig ceiling, while Android sends 270x270
 *                    at quality 75 and routinely exceeds both. Raising that
 *                    cap is the optimisation to make here; it is a change to
 *                    the store's retention bound, not to this file.
 *   content          {"w":1280,"h":960}, optional -- see above.
 *   searchableContent  nothing useful; the digest is the type's name, which
 *                    custom_message_config.c renames to 「[图片]」.
 */

#include "ui_media.h"
#include "ui_page.h"
#include "ui_msg_view.h"

/* One constant with the decoder, so the frame and the picture are the same
 * size and the page does not move when one becomes the other. A picture that
 * reaches both edges of a 320 px panel stops reading as one message among
 * several anyway -- the gap on the far side is what says who sent it, the
 * same reason a bubble stops at 78%. */
#define FRAME_W UI_MEDIA_BOX_W
#define FRAME_H UI_MEDIA_BOX_H

static void draw(lv_obj_t *parent, const ui_msg_row_t *row)
{
    lv_obj_t *line = ui_msg_line(parent, row);

    /* A picture in a group needs the name more than a text bubble does: there
     * is no writing in it to recognise someone by. Shown on every one of them
     * rather than only at the top of a run -- the page's run-of-messages rule
     * is reset around a view, because a view is not a bubble. */
    if (!row->mine && row->group) {
        lv_obj_t *who = lv_label_create(line);

        lv_obj_set_style_text_font(who, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(who, lv_color_hex(UI_C_DIM), 0);
        lv_label_set_text(who, row->who);
    }

    const lv_image_dsc_t *picture = ui_media_get(row->message_uid);

    if (picture != NULL) {
        lv_obj_t *img = lv_image_create(line);

        lv_image_set_src(img, picture);
        lv_obj_set_style_radius(img, 8, 0);
        lv_obj_set_style_clip_corner(img, true, 0);
    } else {
        lv_obj_t *frame = lv_obj_create(line);

        ui_style_flat(frame);
        lv_obj_set_size(frame, FRAME_W, FRAME_H);
        lv_obj_set_style_bg_color(frame, lv_color_hex(UI_C_RAISED), 0);
        lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(frame, 8, 0);
        lv_obj_set_style_border_width(frame, 1, 0);
        lv_obj_set_style_border_color(frame, lv_color_hex(UI_C_LINE), 0);
        lv_obj_set_flex_flow(frame, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(frame, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *icon = lv_label_create(frame);

        lv_obj_set_style_text_color(icon, lv_color_hex(UI_C_DIM), 0);
        lv_label_set_text(icon, LV_SYMBOL_IMAGE);
    }

    /* Last, like every view: the picture and the frame are the same message,
     * and "did they see it" belongs under both (ui_msg_view.h). The early
     * return this used to have is what the if/else replaced. */
    ui_msg_receipt(line, row);
}

const ui_msg_view_t ui_msg_view_image = {
    .type = WFC_CONTENT_IMAGE,
    .draw = draw,
};
