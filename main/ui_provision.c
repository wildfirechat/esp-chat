/* 配网 -- the screen a board shows when it does not know a WiFi network yet.
 *
 * It draws three things: what to do, a QR code that says which access point
 * the board is serving and with what passphrase, and how far the exchange has
 * got. The QR is the whole interface -- the phone scans it, joins that access
 * point and posts a network back (app_net.c) -- but the passphrase is printed
 * underneath it as well, because a phone whose camera will not focus, or an
 * Android old enough to refuse the programmatic join, can still be pointed at
 * the network by hand from the system settings.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ui_page.h"
#include "ui_setup.h"

/* wildfirechat://espwifi/<32-byte SSID>?pwd=<passphrase>, with room to spare. */
#define QR_MAX 128

static SemaphoreHandle_t s_lock;

static char            s_qr[QR_MAX];
static char            s_ap[80];
static char            s_detail[80];
static ui_prov_phase_t s_phase;

static lv_obj_t *s_qrcode;
static lv_obj_t *s_ap_label;
static lv_obj_t *s_status;

/* What is currently encoded in the widget, so that a repaint every 80 ms does
 * not re-run the encoder over the same string. */
static char s_drawn[QR_MAX];

/* Same reason ui_log.c creates its mutex on first use: the first call comes
 * from the boot path, before anything that could have initialised it. */
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

void ui_provision_begin(const char *qr, const char *ap_ssid, const char *ap_password)
{
    if (qr == NULL || !lock()) {
        return;
    }

    strlcpy(s_qr, qr, sizeof(s_qr));
    snprintf(s_ap, sizeof(s_ap), "热点 %s  密码 %s",
             ap_ssid != NULL ? ap_ssid : "-",
             ap_password != NULL ? ap_password : "-");
    s_phase     = UI_PROV_WAIT;
    s_detail[0] = '\0';
    xSemaphoreGive(s_lock);

    ui_dirty(UI_DIRTY_SETUP);
}

void ui_provision_phase(ui_prov_phase_t phase, const char *detail)
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

static void create(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);

    ui_style_flat(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(col, 6, 0);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hint = lv_label_create(col);
    lv_label_set_text(hint, "手机打开野火 IM，扫一扫配网");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_C_TEXT), 0);

    /* A quiet zone in the literal sense: the QR needs white around it to be
     * read at an angle, and this panel's background is nearly black. */
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

    s_ap_label = lv_label_create(col);
    lv_obj_set_style_text_color(s_ap_label, lv_color_hex(UI_C_DIM), 0);
    lv_label_set_text(s_ap_label, "");

    s_status = ui_label(col, "", UI_C_DIM);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
}

static void refresh(uint32_t dirty)
{
    static const struct { const char *text; uint32_t color; } PHASES[] = {
        [UI_PROV_WAIT]   = { "等待手机连接",       UI_C_DIM  },
        [UI_PROV_PHONE]  = { "手机已连上，请选网络", UI_C_TEXT },
        [UI_PROV_TRYING] = { "正在连接",           UI_C_TEXT },
        [UI_PROV_OK]     = { "连上了",             UI_C_OK   },
        [UI_PROV_FAIL]   = { "连接失败",           UI_C_BAD  },
    };

    if ((dirty & UI_DIRTY_SETUP) == 0 || s_qrcode == NULL || !lock()) {
        return;
    }

    if (s_qr[0] != '\0' && strcmp(s_qr, s_drawn) != 0) {
        lv_qrcode_update(s_qrcode, s_qr, strlen(s_qr));
        strlcpy(s_drawn, s_qr, sizeof(s_drawn));
    }
    lv_label_set_text(s_ap_label, s_ap);

    char line[128];

    snprintf(line, sizeof(line), "%s%s%s", PHASES[s_phase].text,
             s_detail[0] != '\0' ? "\n" : "", s_detail);
    lv_label_set_text(s_status, line);
    lv_obj_set_style_text_color(s_status, lv_color_hex(PHASES[s_phase].color), 0);

    xSemaphoreGive(s_lock);
}

static void destroy(void)
{
    s_qrcode   = NULL;
    s_ap_label = NULL;
    s_status   = NULL;
    s_drawn[0] = '\0';
}

static void title(char *buf, size_t buf_size)
{
    strlcpy(buf, "配网", buf_size);
}

const ui_page_def_t ui_page_provision = {
    .create  = create,
    .refresh = refresh,
    .destroy = destroy,
    .title   = title,
    .setup   = true,
};
