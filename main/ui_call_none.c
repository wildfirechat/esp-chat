/* A build with no voice calls.
 *
 * This is ui_call.c's other half: the same functions from ui_page.h and ui.h,
 * none of which do anything, compiled instead of it when CONFIG_APP_CALL=n. That is
 * the arrangement the client already uses for its two store backends -- one
 * header, two implementations, exactly one linked -- and the reason to repeat
 * it here is the same. The shell, the chat page and app_main.c are written
 * once against ui_page.h and have no #ifdef in them; the AV SDK is named in
 * exactly one file, which is ui_call.c and not this one; and a board built
 * without ../wfav-esp beside it is the same application minus a button, not a
 * second version of it that nobody looks at.
 *
 * What is actually lost: the phone button on a single chat (ui_chat.c asks
 * ui_call_available() before it draws it), and an invite that arrives. The
 * invite is not declined -- nothing here subscribes, so the AV SDK is not
 * running and the caller sees the same thing it sees when the board is off.
 *
 * UI_PAGE_CALL still exists, because the page table in ui.c is indexed by
 * ui_page_id_t and a hole in it would be a NULL to check on every switch. It
 * is simply never navigated to: ui_call_open() is the no-op below, so nothing
 * can reach the page, and the empty definition costs one struct in .rodata.
 */

#include <stddef.h>

#include "ui_page.h"

bool ui_call_available(void)
{
    return false;
}

void ui_call_subscribe(void)
{
}

void ui_call_start(void)
{
}

bool ui_call_busy(void)
{
    return false;
}

void ui_call_dial(const char *target)
{
    (void)target;
}

void ui_call_open(void)
{
}

void ui_call_set_end_reason(const char *text)
{
    (void)text;
}

/* Never reached -- ui_call_open() above is the only way in. The three
 * functions are still filled in rather than left NULL: the shell calls
 * create(), refresh() and title() without checking, so a NULL here would turn
 * any future "goto the call page" into a crash instead of a blank screen. */

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

const ui_page_def_t ui_page_call = {
    .create  = create,
    .refresh = refresh,
    .title   = title,
    .home    = false,
};
