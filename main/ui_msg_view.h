/* One message as a page holds it, and the view that draws it.
 *
 * The chat page knows two shapes -- a bubble and a centred notice -- and
 * everything else is a view, looked up by content type. That is the table
 * wfc_content.h keeps for what a type MEANS, on the other side of the screen:
 * what a type LOOKS LIKE.
 *
 * ------------------------------------------------------------------------
 * The row is the envelope, and only the envelope.
 *
 * Nothing in it is type-specific: who sent it, when, whether it is ours,
 * whether the conversation is a group, and one line of text -- the same
 * digest the conversation list shows for the same message. Every type has all
 * of that, and no type has more.
 *
 * That line was drawn after crossing it, so it is worth saying why it is
 * where it is. A row is built inside the store query, which is the one moment
 * a message exists: wfc_message_t BORROWS (wfc_model.h), its strings point
 * into sqlite's row buffer, and the next step() invalidates them. So it is
 * tempting to grab whatever a view might want while it is there -- a URL, a
 * size -- and park it in a type-private corner of the row. Two things go
 * wrong, and both did:
 *
 *   A fixed field clips. The store keeps text up to
 *   CONFIG_WFC_STORE_MAX_TEXT (512 bytes), so a signed media URL copied into
 *   anything smaller is an image that never loads, silently, on the
 *   deployments that issue long URLs and nowhere else.
 *
 *   Half the messages do not have it. The web client writes an image's
 *   dimensions only when it happens to know them, so a frame shaped by them
 *   is a feature that works for some pictures and quietly does not for
 *   others. One shape for all of them is better than that.
 *
 * So a view that needs more than the envelope asks the store for it again --
 * wfc_get_message(row->message_uid, ...), an index lookup on the UID -- and
 * asks from somewhere it is allowed to block, never from draw().
 *
 * That is not about cost. draw() may read the store; it may not BLOCK, and
 * everything the extra fields are wanted for -- fetching a picture, following
 * a link -- blocks. So there is nothing draw() could do with the answer. The
 * page's prime() is one such place, off the display lock. A picture needs
 * more than prime() can give (prime() runs on the UI task, and a download is
 * seconds), so it has a task of its own -- ui_media.h -- but the shape is the
 * same one: draw() asks a question that is always cheap, and what it gets
 * back is either an answer or "not yet".
 *
 * One rule, from a picture's bytes to a work order's status, instead of a
 * second small channel that has to be sized right in advance.
 *
 * ------------------------------------------------------------------------
 * A view is one function. It runs on the UI task with the display lock held:
 * build widgets, read nothing that can block, call nothing in wfc_client.h.
 *
 * A type with no view is drawn by the page -- a centred line when it is
 * registered as a notification, a bubble otherwise -- which is the right
 * answer for text, for the group notices, and for a type nobody has taught
 * this client about. A type that only wants different WORDS needs no view
 * either: change its digest (wfc_content.h) and the conversation list and the
 * bubble stay in step for free.
 *
 * WFC's own types (below 1000) are built in and live beside this file. A
 * deployment's own (1000 and up) live in wfc_custom_message/ and are looked
 * up FIRST, so a fork can replace a built-in view the same way registering a
 * type replaces a built-in type.
 */

#ifndef UI_MSG_VIEW_H
#define UI_MSG_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "wfc_model.h"

/* The longest line a bubble shows. Longer messages are cut on a UTF-8
 * boundary by whoever copies them in -- half a Chinese character draws as a
 * gap, so the copy has to be the one that knows the buffer's size. */
#define UI_MSG_TEXT_MAX 192

/* One message, as the page has it between reading the store and drawing. */
typedef struct {
    int32_t type;
    char    text[UI_MSG_TEXT_MAX];  /* the digest; see the header comment */
    char    who[WFC_NAME_MAX];      /* the sender's display name; ours too */
    char    from[WFC_TARGET_MAX];   /* the sender's ID, "" for our own */
    int64_t timestamp;
    /* The server's ID for this message: stable across repaints and across a
     * reboot, which makes it the key for everything a view needs and the
     * envelope does not carry. 0 before the server names the message. */
    int64_t message_uid;
    bool    mine;
    bool    notice;                 /* a notification type: centred, no bubble */
    bool    group;                  /* the conversation, not the message */
} ui_msg_row_t;

/* One type's view. */
typedef struct {
    int32_t type;
    void (*draw)(lv_obj_t *parent, const ui_msg_row_t *row);
} ui_msg_view_t;

/* Draws the row if its type has a view. False means it does not, and the page
 * falls back to its notice or its bubble. */
bool ui_msg_view_draw(lv_obj_t *parent, const ui_msg_row_t *row);

/* ------------------------------------------------------------- the views */

/* WFC's image type (3). ui_msg_image.c. */
extern const ui_msg_view_t ui_msg_view_image;

/* --------------------------------------------------------------- helper */

/* The container a message sits in: full width, its content pushed to our side
 * or theirs. Every view starts with one of these, and so does the page's own
 * bubble -- getting the alignment wrong is what makes a message look like it
 * came from the wrong person. */
lv_obj_t *ui_msg_line(lv_obj_t *parent, const ui_msg_row_t *row);

#endif /* UI_MSG_VIEW_H */
