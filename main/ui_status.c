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
#include "esp_timer.h"

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
}

static void destroy(void)
{
    memset(s_value, 0, sizeof(s_value));
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
