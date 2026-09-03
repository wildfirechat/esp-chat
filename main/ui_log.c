/* 日志 -- the running log, which is the page you look at when something is
 * wrong.
 *
 * Unlike everything else on the panel this has no model behind it in the
 * store, so it keeps its own: a ring of lines, written by whichever task
 * logged them and read by the UI task when the page repaints. That is why it
 * does not go through ui.c's queue like the other pushed values do -- a
 * catch-up delivering forty messages would overrun a queue sized for
 * occasional updates and drop exactly the lines worth reading, whereas a ring
 * drops the oldest, which is what a log is supposed to do.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ui_page.h"

/* Enough to cover a boot and the first exchange. Every line is also on the
 * serial console, which is where a real investigation happens. */
#define LOG_ROWS 32
#define LOG_LEN  144

static char          s_lines[LOG_ROWS][LOG_LEN];
static ui_log_kind_t s_kinds[LOG_ROWS];
static size_t        s_next;      /* where the next line goes */
static size_t        s_count;

static SemaphoreHandle_t s_lock;
static lv_obj_t         *s_list;

/* Created on first use rather than in an init call there is no good place to
 * make -- the first lines are logged from app_main before any other task
 * exists, which is the same reasoning wfc_event.c uses. */
static bool lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return false;
        }
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

void ui_log_append(ui_log_kind_t kind, const char *line)
{
    if (line == NULL || !lock()) {
        return;
    }

    strlcpy(s_lines[s_next], line, LOG_LEN);
    s_kinds[s_next] = kind;
    s_next          = (s_next + 1) % LOG_ROWS;
    if (s_count < LOG_ROWS) {
        s_count++;
    }
    xSemaphoreGive(s_lock);

    ui_dirty(UI_DIRTY_LOG);
}

/* ---------------------------------------------------------------- drawing */

static void create(lv_obj_t *parent)
{
    s_list = lv_obj_create(parent);
    ui_style_flat(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
}

static void refresh(uint32_t dirty)
{
    static const uint32_t COLORS[] = {
        [UI_LOG_NOTE]  = UI_C_DIM,
        [UI_LOG_IN]    = UI_C_IN,
        [UI_LOG_OUT]   = UI_C_TEXT,
        [UI_LOG_ERROR] = UI_C_BAD,
    };

    if ((dirty & UI_DIRTY_LOG) == 0 || !lock()) {
        return;
    }

    lv_obj_clean(s_list);
    for (size_t i = 0; i < s_count; i++) {
        /* The ring holds the newest at s_next - 1; the page reads top to
         * bottom, oldest first. */
        size_t slot = (s_next + LOG_ROWS - s_count + i) % LOG_ROWS;

        ui_label(s_list, s_lines[slot], COLORS[s_kinds[slot]]);
    }
    xSemaphoreGive(s_lock);

    if (lv_obj_get_child_count(s_list) > 0) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_view(lv_obj_get_child(s_list, -1), LV_ANIM_OFF);
    }
}

static void destroy(void)
{
    s_list = NULL;
}

static void title(char *buf, size_t buf_size)
{
    strlcpy(buf, "日志", buf_size);
}

const ui_page_def_t ui_page_log = {
    .create  = create,
    .refresh = refresh,
    .destroy = destroy,
    .title   = title,
    .home    = true,
};
