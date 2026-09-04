/* A build with no push-to-talk.
 *
 * ui_ptt.c and app_ptt.c's other half: the same functions from ui_page.h and
 * ui.h, none of which do anything, compiled instead of them when
 * CONFIG_APP_PTT=n. Same arrangement as ui_call_none.c, and the reasons are
 * the same one for one -- the shell and the chat page are written once
 * against ui_page.h with no #ifdef in them, wfptt.h is named in exactly two
 * files and neither is this one, and a board built without ../wfptt-esp
 * beside it is the same application minus a button.
 *
 * What is lost: the 对讲 button on the chat page (ui_chat.c asks
 * ui_ptt_available() before it draws it), and anyone pressing theirs. Nothing
 * here subscribes, so a talk arrives at a board that is not listening, which
 * to the person talking looks exactly like a board that is switched off.
 *
 * Voice messages left behind by other people's talks (content type 23) still
 * play, and that is not an accident: ui_msg_voice.c draws them, and it does
 * not depend on this module at all.
 *
 * UI_PAGE_PTT still exists, because the page table in ui.c is indexed by
 * ui_page_id_t and a hole in it would be a NULL to check on every switch.
 */

#include <stddef.h>

#include "ui_page.h"

bool ui_ptt_available(void)
{
    return false;
}

void ui_ptt_start(void)
{
}

bool ui_ptt_busy(void)
{
    return false;
}

void ui_ptt_open(const wfc_conversation_t *conv)
{
    (void)conv;
}

/* Never reached -- ui_ptt_open() above is the only way in. Filled in rather
 * than left NULL for the reason ui_call_none.c gives: the shell calls
 * create(), refresh() and title() without checking. */

static void create(lv_obj_t *parent)
{
    (void)parent;
}

static void refresh(uint32_t dirty)
{
    (void)dirty;
}

static void title(char *buf, size_t buf_size)
{
    if (buf_size > 0) {
        buf[0] = '\0';
    }
}

const ui_page_def_t ui_page_ptt = {
    .create  = create,
    .refresh = refresh,
    .title   = title,
    .home    = false,
};
