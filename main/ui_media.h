/* Pictures: where the bytes come from, and who is allowed to touch them.
 *
 * A view draws from ui_msg_row_t, which is the envelope and nothing else
 * (ui_msg_view.h). A picture is the first thing that needs more than the
 * envelope, and it needs a great deal more: the URL out of the store, a few
 * hundred KB over HTTP, a JPEG decode, and somewhere to keep the pixels until
 * the conversation is closed. None of that fits in a row and none of it can
 * happen where a view runs.
 *
 * So it happens here, on a task of this file's own, and a view asks a
 * question that is always cheap to answer:
 *
 *     ui_media_get(uid)  ->  something drawable, or NULL
 *
 * NULL means "not yet" rather than "no": the ask is what puts the message on
 * the fetcher's list, and when the picture lands the screen is marked dirty
 * and the next repaint draws it. A view therefore has exactly one job -- draw
 * the picture if there is one, draw its frame if there is not -- and no
 * concept of loading, retrying or failing.
 *
 * ------------------------------------------------------------------------
 * Why a task rather than the page's prime().
 *
 * prime() is where the chat page does its blocking work, and it would be the
 * natural home for this. But prime() runs on the UI task, and that task is
 * also the one that processes navigation: a fetch that takes four seconds is
 * four seconds where the back arrow does nothing. Fetching a picture is not
 * like asking for a display name -- it is a download and a decode, and it
 * belongs off that task entirely.
 *
 * The task is created on the first picture anyone actually looks at, not at
 * startup. Its stack is internal SRAM, which is the pool this board runs out
 * of first (see sdkconfig.defaults), and a board that never opens a
 * conversation with a picture in it should not pay for one.
 *
 * ------------------------------------------------------------------------
 * The one rule that matters: WHO MAY FREE A PICTURE.
 *
 * ui_media_get() hands out a pointer that LVGL keeps for as long as the image
 * widget lives. Freeing it while a widget still points at it draws freed
 * memory. So the fetcher never frees anything a view can see -- it only ever
 * fills an empty slot -- and freeing happens in exactly one place:
 * ui_media_keep_only(), called by the page from refresh() AFTER it has
 * emptied the message list and BEFORE it builds the new one. At that moment
 * no widget holds a pointer, and it is the only such moment.
 */

#ifndef UI_MEDIA_H
#define UI_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/* The box a picture is decoded to fit inside, and therefore the size of the
 * frame a view draws while it is waiting -- one constant so the layout does
 * not move when the picture arrives.
 *
 * Kept small deliberately. The JPEG is scaled down during the decode, not
 * after it, so this is what decides the memory a picture costs: at 132x100 in
 * RGB565 a cached picture is about 26 KB. */
#define UI_MEDIA_BOX_W 132
#define UI_MEDIA_BOX_H 100

/* Creates the lock and the queue. Once, from ui_init(). Cheap -- the fetcher
 * task is not created until something is actually asked for. */
void ui_media_start(void);

/* The picture for this message, or NULL if there is not one yet.
 *
 * Called from a view's draw(): UI task, display lock held. Never blocks, not
 * even on this file's own lock -- a repaint that has to wait for a download
 * to finish is worse than a repaint that draws the frame again.
 *
 * A miss puts the message on the fetcher's list. A message that has already
 * failed stays failed for as long as it is on screen, so a picture that is
 * not there does not become a request every repaint. */
const lv_image_dsc_t *ui_media_get(int64_t message_uid);

/* Frees every picture whose message is not in `uids`.
 *
 * The page calls this from refresh(), right after lv_obj_clean() and before
 * it draws anything -- see the header comment for why that is the only safe
 * moment. Leaving a conversation calls it with n = 0. */
void ui_media_keep_only(const int64_t *uids, size_t n);

/* ------------------------------------------------- the two shared halves
 *
 * Pictures are the first thing that needed bytes from a URL, not the only
 * one: a voice message needs the same two steps -- find the URL a message
 * points at, then get what is behind it -- and differs only in what it does
 * with the result. Both live here because this file is the media layer, and
 * because the download in particular is not the ten lines it looks like: it
 * follows redirects by hand (esp_http_client will not, in the mode that lets
 * the body go straight into one PSRAM buffer) and refuses a response with no
 * Content-Length.
 *
 * Both block, and neither may be called from the UI task. */

/* The remote URL a message points at, wfc_strdup'd, or NULL. */
char *ui_media_url(int64_t message_uid);

/* The whole body, in PSRAM, or NULL. Caller frees with wfc_free(). */
uint8_t *ui_media_download(const char *url, size_t *out_len);

#endif /* UI_MEDIA_H */
