/* 扫码登录 -- the screen a board shows when it has a network but no account.
 *
 * The same shape as the 配网 page next door, and deliberately so: a line
 * saying what to do, a QR code, and a status line under it. What differs is
 * where the QR comes from. This one is a login session the app server minted
 * (app_login.c), the same wildfirechat://pcsession/<token> a desktop client
 * shows, so the phone recognises it with no changes at all -- it opens the
 * "允许登录" sheet it already has and confirms against the same endpoints.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_timer.h"

#include "app_cfg.h"
#include "ui_page.h"
#include "ui_setup.h"

/* wildfirechat://pcsession/ plus a UUID is 58 bytes. */
#define QR_MAX 128

static SemaphoreHandle_t s_lock;

static char             s_qr[QR_MAX];
static char             s_detail[80];
static ui_login_phase_t s_phase;

static lv_obj_t *s_qrcode;
static lv_obj_t *s_status;

/* The way out of the one corner this page has. Everything it does needs the
 * app server, so a board that was provisioned onto a network with no route to
 * it -- a guest SSID behind a portal, the wrong one of two similar names --
 * sits here saying "服务器没有响应" until the hour is up, and the only other
 * cure is a USB cable and an NVS erase. So the page carries the same 重新配网
 * button the 状态 page has, with the same two-tap confirmation, because a
 * mis-tap here throws away a network that may well be fine. */
#define CONFIRM_WINDOW_MS 5000

static lv_obj_t *s_forget;
static int64_t   s_armed_ms;

static char s_drawn[QR_MAX];

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

void ui_login_begin(const char *qr)
{
    if (qr == NULL || !lock()) {
        return;
    }

    strlcpy(s_qr, qr, sizeof(s_qr));
    s_phase     = UI_LOGIN_WAIT;
    s_detail[0] = '\0';
    xSemaphoreGive(s_lock);

    ui_dirty(UI_DIRTY_SETUP);
}

void ui_login_phase(ui_login_phase_t phase, const char *detail)
{
    if (!lock()) {
        return;
    }

    s_phase = phase;
    strlcpy(s_detail, detail != NULL ? detail : "", sizeof(s_detail));
    xSemaphoreGive(s_lock);

    ui_dirty(UI_DIRTY_SETUP);
}

/* ---------------------------------------------------------------- drawing */

/* On the LVGL task with the display lock held, like every button callback
 * (ui_page.h). An NVS write is a few milliseconds and a restart ends the
 * argument; there is no client to disconnect from at this point anyway. */
static void forget_wifi(lv_event_t *e)
{
    (void)e;

    int64_t now = esp_timer_get_time() / 1000;

    if (s_armed_ms == 0 || now - s_armed_ms >= CONFIRM_WINDOW_MS) {
        s_armed_ms = now;
        lv_label_set_text(s_forget, "再按一次");
        lv_obj_set_style_text_color(s_forget, lv_color_hex(UI_C_BAD), 0);
        return;
    }

    app_cfg_clear_wifi();
    esp_restart();
}

static void create(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);

    ui_style_flat(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(col, 6, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hint = lv_label_create(col);
    lv_label_set_text(hint, "手机打开野火 IM，扫一扫登录");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_C_TEXT), 0);

    lv_obj_t *card = lv_obj_create(col);
    ui_style_flat(card);
    lv_obj_set_size(card, 136, 136);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_qrcode = lv_qrcode_create(card);
    lv_qrcode_set_size(s_qrcode, 120);
    lv_qrcode_set_dark_color(s_qrcode, lv_color_black());
    lv_qrcode_set_light_color(s_qrcode, lv_color_white());
    s_drawn[0] = '\0';

    s_status = ui_label(col, "", UI_C_DIM);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn = lv_button_create(col);

    lv_obj_set_size(btn, 100, 26);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_add_event_cb(btn, forget_wifi, LV_EVENT_CLICKED, NULL);

    s_forget   = lv_label_create(btn);
    s_armed_ms = 0;
    lv_label_set_text(s_forget, "重新配网");
    lv_obj_set_style_text_color(s_forget, lv_color_hex(UI_C_DIM), 0);
    lv_obj_center(s_forget);
}

static void refresh(uint32_t dirty)
{
    static const struct { const char *text; uint32_t color; } PHASES[] = {
        [UI_LOGIN_WAIT]    = { "等待手机扫码",         UI_C_DIM  },
        [UI_LOGIN_SCANNED] = { "已扫码，请在手机上确认", UI_C_TEXT },
        [UI_LOGIN_OK]      = { "登录成功",             UI_C_OK   },
        [UI_LOGIN_FAIL]    = { "登录失败",             UI_C_BAD  },
    };

    /* STATUS as well as SETUP: it is the bit the shell sets once a second,
     * which is what disarms the button below without a timer of its own. */
    if ((dirty & (UI_DIRTY_SETUP | UI_DIRTY_STATUS)) == 0 || s_qrcode == NULL ||
        !lock()) {
        return;
    }

    if (s_qr[0] != '\0' && strcmp(s_qr, s_drawn) != 0) {
        lv_qrcode_update(s_qrcode, s_qr, strlen(s_qr));
        strlcpy(s_drawn, s_qr, sizeof(s_drawn));
    }

    char line[128];

    snprintf(line, sizeof(line), "%s%s%s", PHASES[s_phase].text,
             s_detail[0] != '\0' ? "\n" : "", s_detail);
    lv_label_set_text(s_status, line);
    lv_obj_set_style_text_color(s_status, lv_color_hex(PHASES[s_phase].color), 0);

    xSemaphoreGive(s_lock);

    /* An armed button that was then left alone goes back to saying what it
     * does. The repaint is once a second whatever else happens (ui.c). */
    if (s_armed_ms != 0 && esp_timer_get_time() / 1000 - s_armed_ms >= CONFIRM_WINDOW_MS) {
        s_armed_ms = 0;
        lv_label_set_text(s_forget, "重新配网");
        lv_obj_set_style_text_color(s_forget, lv_color_hex(UI_C_DIM), 0);
    }
}

static void destroy(void)
{
    s_qrcode   = NULL;
    s_status   = NULL;
    s_forget   = NULL;
    s_armed_ms = 0;
    s_drawn[0] = '\0';
}

static void title(char *buf, size_t buf_size)
{
    strlcpy(buf, "扫码登录", buf_size);
}

const ui_page_def_t ui_page_login = {
    .create  = create,
    .refresh = refresh,
    .destroy = destroy,
    .title   = title,
    .setup   = true,
};
