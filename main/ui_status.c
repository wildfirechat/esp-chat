/* 状态 -- what the board is doing, for when the screen is the only thing you
 * have.
 *
 * Almost every row is read from the client at repaint time rather than pushed
 * in, which is why there is so little plumbing left in app_main.c: the store
 * knows how many messages it holds, the client knows where the sync head is,
 * and the heap knows how much of it is left. The three exceptions are the
 * three things the client genuinely does not know -- the WiFi it is on, the
 * address DHCP gave it, and the account it was configured with -- and those
 * arrive through ui.h and are kept here, beside the widgets rather than in
 * them, so they survive leaving the page and coming back.
 */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_cfg.h"
#include "ui_page.h"
#include "wfc_store.h"

typedef enum {
    ROW_WIFI = 0,
    ROW_IP,
    ROW_ACCOUNT,
    ROW_LINK,
    ROW_MESSAGES,
    ROW_HEAD,
    ROW_STORE,
    ROW_CONVS,
    ROW_MEMORY,
    ROW_UPTIME,
    ROW_COUNT,
} row_t;

static const char *const ROW_NAMES[ROW_COUNT] = {
    [ROW_WIFI]     = "WiFi",
    [ROW_IP]       = "IP",
    [ROW_ACCOUNT]  = "账号",
    [ROW_LINK]     = "长连接",
    [ROW_MESSAGES] = "收发",
    [ROW_HEAD]     = "同步位置",
    [ROW_STORE]    = "存储",
    [ROW_CONVS]    = "会话",
    [ROW_MEMORY]   = "内存",
    [ROW_UPTIME]   = "运行",
};

static lv_obj_t *s_value[ROW_COUNT];

/* The two buttons at the bottom, and the one piece of state they need: when
 * each was armed. Clearing the network or the account cannot be undone from
 * the panel -- the board restarts into a QR code and needs a phone to get
 * back -- so neither happens on one tap. The first tap arms the button and
 * relabels it, and it disarms itself if the second tap does not come.
 *
 * A confirmation dialog would be the other way to do this, and it would cost
 * a modal, a backdrop and two more Chinese strings on a screen where the
 * finger is the only pointer; two taps says the same thing. */
#define CONFIRM_WINDOW_MS 5000

typedef struct {
    lv_obj_t   *label;
    const char *idle;
    int64_t     armed_ms;
} confirm_t;

static confirm_t s_forget_wifi    = { .idle = "重新配网" };
static confirm_t s_forget_account = { .idle = "退出登录" };

/* Pushed in from the application; the UI task is the only writer. */
static char s_wifi[48]    = "-";
static char s_ip[24]      = "-";
static char s_account[48] = "-";

/* When the long link came up, so the page can say how long it has held. Kept
 * here because the client reports a status, not a duration. */
static int64_t s_link_up_ms;

void ui_status_set_wifi(const char *ssid, int rssi)
{
    if (ssid == NULL || ssid[0] == '\0') {
        strlcpy(s_wifi, "未连接", sizeof(s_wifi));
    } else {
        snprintf(s_wifi, sizeof(s_wifi), "%s  %d dBm", ssid, rssi);
    }
    ui_dirty(UI_DIRTY_STATUS);
}

void ui_status_set_ip(const char *ip)
{
    strlcpy(s_ip, ip != NULL && ip[0] != '\0' ? ip : "-", sizeof(s_ip));
    ui_dirty(UI_DIRTY_STATUS);
}

void ui_status_set_account(const char *user_id)
{
    strlcpy(s_account, user_id != NULL && user_id[0] != '\0' ? user_id : "-",
            sizeof(s_account));
    ui_dirty(UI_DIRTY_STATUS);
}

/* ---------------------------------------------------------------- buttons */

/* Runs on the LVGL task with the display lock held, like every other button
 * callback here (ui_page.h), so the rule is the usual one: no blocking and no
 * calls into the client. An NVS write is neither -- it is a few milliseconds
 * of flash -- and what follows it is a restart, which ends the argument. */
static bool armed(confirm_t *button)
{
    int64_t now = esp_timer_get_time() / 1000;

    if (button->armed_ms != 0 && now - button->armed_ms < CONFIRM_WINDOW_MS) {
        return true;
    }

    button->armed_ms = now;
    lv_label_set_text(button->label, "再按一次");
    lv_obj_set_style_text_color(button->label, lv_color_hex(UI_C_BAD), 0);
    return false;
}

/* No wfc_client_disconnect() before either of these. It waits for the client's
 * own tasks, and waiting for anything from a button callback is what freezes
 * the panel; the server treats a board that vanishes the same way it treats
 * one that said goodbye. */
static void forget_wifi(lv_event_t *e)
{
    (void)e;

    if (!armed(&s_forget_wifi)) {
        return;
    }
    app_cfg_clear_wifi();
    esp_restart();
}

static void forget_account(lv_event_t *e)
{
    (void)e;

    if (!armed(&s_forget_account)) {
        return;
    }
    app_cfg_clear_account();
    esp_restart();
}

static void build_button(lv_obj_t *parent, confirm_t *button, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_height(btn, 28);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_RAISED), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    button->label    = lv_label_create(btn);
    button->armed_ms = 0;
    lv_label_set_text(button->label, button->idle);
    lv_obj_set_style_text_color(button->label, lv_color_hex(UI_C_DIM), 0);
    lv_obj_center(button->label);
}

/* Called from the repaint, which is once a second here: a button that was
 * armed and then left alone goes back to saying what it does. */
static void disarm_stale(confirm_t *button)
{
    if (button->label == NULL || button->armed_ms == 0) {
        return;
    }
    if (esp_timer_get_time() / 1000 - button->armed_ms < CONFIRM_WINDOW_MS) {
        return;
    }

    button->armed_ms = 0;
    lv_label_set_text(button->label, button->idle);
    lv_obj_set_style_text_color(button->label, lv_color_hex(UI_C_DIM), 0);
}

/* ---------------------------------------------------------------- drawing */

static void set(row_t row, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set(row_t row, const char *fmt, ...)
{
    char    buf[64];
    va_list args;

    if (s_value[row] == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    lv_label_set_text(s_value[row], buf);
}

static void create(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);

    ui_style_flat(list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(list);

        ui_style_flat(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 8, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_width(name, 76);
        lv_label_set_text(name, ROW_NAMES[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_C_DIM), 0);

        lv_obj_t *value = lv_label_create(row);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(value, 1);
        lv_label_set_text(value, "-");
        lv_obj_set_style_text_color(value, lv_color_hex(UI_C_TEXT), 0);

        s_value[i] = value;
    }

    lv_obj_t *buttons = lv_obj_create(list);

    ui_style_flat(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(buttons, 8, 0);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);

    build_button(buttons, &s_forget_wifi, forget_wifi);
    build_button(buttons, &s_forget_account, forget_account);
}

static void refresh(uint32_t dirty)
{
    if ((dirty & (UI_DIRTY_STATUS | UI_DIRTY_CONVS | UI_DIRTY_MESSAGES)) == 0) {
        return;
    }

    wfc_connection_status_t status = wfc_client_status();
    bool up = status == WFC_STATUS_CONNECTED || status == WFC_STATUS_RECEIVING;

    if (up && s_link_up_ms == 0) {
        s_link_up_ms = esp_timer_get_time() / 1000;
    } else if (!up) {
        s_link_up_ms = 0;
    }

    set(ROW_WIFI, "%s", s_wifi);
    set(ROW_IP, "%s", s_ip);
    set(ROW_ACCOUNT, "%s", s_account);

    if (s_link_up_ms != 0) {
        unsigned held = (unsigned)((esp_timer_get_time() / 1000 - s_link_up_ms) / 1000);

        set(ROW_LINK, "%s %02u:%02u:%02u", wfc_is_syncing() ? "同步中" : "已连接",
            held / 3600, held / 60 % 60, held % 60);
    } else {
        set(ROW_LINK, "%s", wfc_status_str(status));
    }

    set(ROW_MESSAGES, "收 %u / 发 %u", (unsigned)wfc_received_count(),
        (unsigned)wfc_sent_count());
    set(ROW_HEAD, "%lld", (long long)wfc_message_head());
    set(ROW_STORE, "%s · %u 条%s", wfc_store_name(),
        (unsigned)wfc_store_message_count(),
        wfc_store_is_persistent() ? "" : "（不落盘）");
    set(ROW_CONVS, "%u 个 · 未读 %u", (unsigned)wfc_store_conversation_count(),
        (unsigned)wfc_get_unread_count());
    set(ROW_MEMORY, "%uK 内部 / %uK PSRAM",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    unsigned uptime = (unsigned)(esp_timer_get_time() / 1000000);
    set(ROW_UPTIME, "%02u:%02u:%02u", uptime / 3600, uptime / 60 % 60, uptime % 60);

    disarm_stale(&s_forget_wifi);
    disarm_stale(&s_forget_account);
}

static void destroy(void)
{
    memset(s_value, 0, sizeof(s_value));
    s_forget_wifi.label       = NULL;
    s_forget_wifi.armed_ms    = 0;
    s_forget_account.label    = NULL;
    s_forget_account.armed_ms = 0;
}

static void title(char *buf, size_t buf_size)
{
    strlcpy(buf, "状态", buf_size);
}

const ui_page_def_t ui_page_status = {
    .create  = create,
    .refresh = refresh,
    .destroy = destroy,
    .title   = title,
    .home    = true,
};
