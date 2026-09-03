/* The shell: one header, one content area, one nav bar, and the bridge from
 * the client's events to a repaint.
 *
 * Three things live here that nowhere else can own.
 *
 * The router. A page is created, filled, and deleted whole (ui_page.h); this
 * holds the stack that makes 会话 -> chat -> 写 work, and 联系人 -> 一个联系人
 * -> chat -> 写 with it, and switching happens on the UI task even when a
 * button on the LVGL task asked for it, so no page is ever deleted from
 * inside its own event callback.
 *
 * The repaint tick. Events arrive in bursts -- a catch-up delivers forty
 * messages, each one moving a conversation to the top of the list -- and
 * repainting per event would spend the whole burst redrawing. So events set
 * dirty bits and the UI task repaints at most once every UI_TICK_MS with all
 * of them collected. This is the same reason wfc_client.h has a RECEIVING
 * status distinct from CONNECTED.
 *
 * The subscriptions. They are made once, here, and never cancelled -- which
 * looks like it ignores what wfc_subscription_t is for, and is worth saying
 * why. A per-page subscription would not protect a page from an event that
 * arrives while it is being destroyed, because the event does not reach the
 * page directly: it sets a bit, and the UI task repaints whatever page is
 * current when it next runs. Cancelling on the wfc task cannot make that race
 * smaller, because the race is in the queue, not in the subscription list.
 * Resolving "which page" at repaint time removes it instead.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp/esp-box-3.h"
#include "lvgl.h"

#include "ui.h"
#include "ui_page.h"
#include "wfc_font.h"

#include "ui_media.h"

static const char *TAG = "ui";

/* How often the UI task may repaint. Fast enough that a message appears the
 * moment it lands, slow enough that a forty-message catch-up costs a handful
 * of redraws rather than forty. */
#define UI_TICK_MS   80

#define UI_TEXT_MAX  256

/* Queue events. Everything here is rare -- a link state change, an IP, a
 * navigation -- which is why they are queued at all. The message log goes the
 * other way, straight into ui_log.c's ring under its own mutex, because a
 * catch-up would overrun a queue this size and drop exactly the lines worth
 * reading. */
enum {
    EVT_LINK,      /* text, flag */
    EVT_WIFI,      /* text = SSID or NULL, num = dBm */
    EVT_IP,        /* text */
    EVT_ACCOUNT,   /* text */
    EVT_GOTO,      /* slot = ui_page_id_t */
    EVT_BACK,
    EVT_CALL_ENDED, /* text = why, for the call page's title */
};

typedef struct {
    uint8_t  type;
    uint8_t  slot;
    bool     flag;
    int32_t  num;
    char    *text;   /* malloc'd; the UI task frees it */
} ui_evt_t;

static QueueHandle_t s_queue;
static volatile uint32_t s_dirty;

static lv_obj_t *s_header_title;
static lv_obj_t *s_header_back;
static lv_obj_t *s_link_dot;
static lv_obj_t *s_clock;
static lv_obj_t *s_content;
static lv_obj_t *s_navbar;

/* The home pages, in the order the bar shows them. */
#define NAV_COUNT 4

static lv_obj_t *s_nav_btn[NAV_COUNT];

static const ui_page_def_t *s_def;
static ui_page_id_t         s_page = UI_PAGE_CONVS;
static ui_page_id_t         s_home = UI_PAGE_CONVS;
/* Deep enough for the longest path the pages can build: 联系人 -> a contact
 * -> that contact's chat -> the composer, with a call able to arrive on top
 * of any of them. */
static ui_page_id_t         s_stack[4];
static int                  s_depth;

static const ui_page_def_t *const PAGES[UI_PAGE_COUNT] = {
    [UI_PAGE_CONVS]    = &ui_page_convs,
    [UI_PAGE_CONTACTS] = &ui_page_contacts,
    [UI_PAGE_STATUS]   = &ui_page_status,
    [UI_PAGE_LOG]      = &ui_page_log,
    [UI_PAGE_CHAT]     = &ui_page_chat,
    [UI_PAGE_CONTACT]  = &ui_page_contact,
    [UI_PAGE_COMPOSE]  = &ui_page_compose,
    [UI_PAGE_CALL]     = &ui_page_call,
};

static const char *const NAV_NAMES[NAV_COUNT] = { "会话", "联系人", "状态", "日志" };
static const ui_page_id_t NAV_PAGES[NAV_COUNT] = {
    UI_PAGE_CONVS, UI_PAGE_CONTACTS, UI_PAGE_STATUS, UI_PAGE_LOG,
};

/* ------------------------------------------------------------- helpers */

void ui_style_flat(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_C_BG), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_row(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_column(lv_obj_t *parent, int32_t pad)
{
    lv_obj_t *obj = lv_obj_create(parent);

    ui_style_flat(obj);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(obj, pad, 0);
    return obj;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/* WFC stamps are server milliseconds. A conversation row has room for about
 * five characters, so the rule is the one every messaging app uses: the time
 * if it is today, "昨天" if it was, otherwise the date. */
void ui_format_time(int64_t ms, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (ms <= 0) {
        buf[0] = '\0';
        return;
    }

    time_t    when = (time_t)(ms / 1000);
    time_t    now  = time(NULL);
    struct tm when_tm;
    struct tm now_tm;

    localtime_r(&when, &when_tm);
    localtime_r(&now, &now_tm);

    int days = when_tm.tm_yday - now_tm.tm_yday;
    if (when_tm.tm_year == now_tm.tm_year && days == 0) {
        strftime(buf, buf_size, "%H:%M", &when_tm);
    } else if (when_tm.tm_year == now_tm.tm_year && days == -1) {
        strftime(buf, buf_size, "昨天 %H:%M", &when_tm);
    } else {
        strftime(buf, buf_size, "%m月%d日", &when_tm);
    }
}

/* ---------------------------------------------------------------- header */

static void back_clicked(lv_event_t *e)
{
    (void)e;
    ui_back();
}

static void nav_clicked(lv_event_t *e)
{
    ui_goto((ui_page_id_t)(uintptr_t)lv_event_get_user_data(e));
}

static void build_header(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);

    ui_style_flat(header);
    lv_obj_set_size(header, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(header, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(UI_C_LINE), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_pad_hor(header, 6, 0);
    lv_obj_set_style_pad_column(header, 6, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* A wide, obvious touch target: the back arrow is the control people
     * reach for most and a 320x240 panel is operated with a fingertip. */
    s_header_back = lv_button_create(header);
    lv_obj_set_size(s_header_back, 34, 24);
    lv_obj_set_style_bg_opa(s_header_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_header_back, 0, 0);
    lv_obj_set_style_pad_all(s_header_back, 0, 0);
    lv_obj_add_event_cb(s_header_back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(s_header_back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(UI_C_ACCENT), 0);
    lv_obj_center(arrow);

    s_header_title = lv_label_create(header);
    lv_label_set_long_mode(s_header_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(s_header_title, 1);
    lv_obj_set_style_text_color(s_header_title, lv_color_hex(UI_C_TEXT), 0);
    lv_label_set_text(s_header_title, "野火 IM");

    s_link_dot = lv_label_create(header);
    lv_label_set_text(s_link_dot, "●");
    lv_obj_set_style_text_color(s_link_dot, lv_color_hex(UI_C_DIM), 0);

    s_clock = lv_label_create(header);
    lv_obj_set_style_text_font(s_clock, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(UI_C_DIM), 0);
    lv_label_set_text(s_clock, "--:--");
}

static void build_navbar(lv_obj_t *parent)
{
    s_navbar = lv_obj_create(parent);

    ui_style_flat(s_navbar);
    lv_obj_set_size(s_navbar, LV_PCT(100), 32);
    lv_obj_set_style_bg_color(s_navbar, lv_color_hex(UI_C_PANEL), 0);
    lv_obj_set_style_border_side(s_navbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_navbar, lv_color_hex(UI_C_LINE), 0);
    lv_obj_set_style_border_width(s_navbar, 1, 0);
    lv_obj_set_flex_flow(s_navbar, LV_FLEX_FLOW_ROW);

    for (int i = 0; i < NAV_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_navbar);

        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_add_event_cb(btn, nav_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)NAV_PAGES[i]);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, NAV_NAMES[i]);
        lv_obj_center(label);

        s_nav_btn[i] = label;
    }
}

/* ---------------------------------------------------------------- router */

/* The title is not fixed for the life of a page: the conversation list puts
 * the unread total in it, and a chat page's name arrives with the profile. So
 * it is re-read after every repaint, not only on a page switch. */
static void apply_title(void)
{
    char title[UI_TITLE_MAX] = "";

    if (s_def->title != NULL) {
        s_def->title(title, sizeof(title));
    }
    lv_label_set_text(s_header_title, title);
}

static void apply_chrome(void)
{
    apply_title();

    if (s_def->home) {
        lv_obj_add_flag(s_header_back, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_navbar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_header_back, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_navbar, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < NAV_COUNT; i++) {
        bool current = NAV_PAGES[i] == s_page;

        lv_obj_set_style_text_color(
            s_nav_btn[i], lv_color_hex(current ? UI_C_ACCENT : UI_C_DIM), 0);
    }
}

/* Deletes the outgoing page whole and builds the incoming one. Only ever
 * called from the UI task, which is the point of routing through the queue:
 * a row's click callback runs on the LVGL task, and deleting the row from
 * inside its own event handler is how LVGL crashes. */
static void switch_to(ui_page_id_t page)
{
    if (s_def != NULL && s_def->destroy != NULL) {
        s_def->destroy();
    }
    lv_obj_clean(s_content);

    /* Reset the layout the last page may have changed: the composer turns it
     * off so the IME can place its candidate bar itself. */
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);

    s_page = page;
    s_def  = PAGES[page];
    s_def->create(s_content);
    apply_chrome();
    s_def->refresh(UI_DIRTY_ALL);
}

ui_page_id_t ui_current_page(void)
{
    return s_page;
}

void ui_goto(ui_page_id_t page)
{
    ui_evt_t evt = { .type = EVT_GOTO, .slot = (uint8_t)page };

    if (s_queue != NULL) {
        xQueueSend(s_queue, &evt, 0);
    }
}

void ui_back(void)
{
    ui_evt_t evt = { .type = EVT_BACK };

    if (s_queue != NULL) {
        xQueueSend(s_queue, &evt, 0);
    }
}

static void nav_goto(ui_page_id_t page)
{
    /* An invite arriving while the call page is already up -- a second
     * caller, or a repeat of the same one -- must not push a second copy of
     * it onto the stack, or the back arrow would have to be pressed twice to
     * get out of one call. */
    if (page == UI_PAGE_CALL && s_page == UI_PAGE_CALL) {
        return;
    }

    if (PAGES[page]->home) {
        s_home  = page;
        s_depth = 0;
    } else if (s_depth < (int)(sizeof(s_stack) / sizeof(s_stack[0]))) {
        s_stack[s_depth++] = page;
    } else {
        /* Deeper than the shell was built for. Replacing the top rather than
         * dropping the request keeps the back arrow meaningful. */
        s_stack[s_depth - 1] = page;
    }
    switch_to(page);
}

static void nav_back(void)
{
    if (s_depth > 0) {
        s_depth--;
    }
    switch_to(s_depth > 0 ? s_stack[s_depth - 1] : s_home);
}

/* ------------------------------------------------------------ the tick */

/* The clock in the header is the one thing that has to keep moving whatever
 * page is up, so it is updated here rather than by a page. Everything else
 * the status page shows is re-read when it repaints. */
static void tick_clock(void)
{
    time_t    now = time(NULL);
    struct tm tm;
    char      buf[16];

    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    lv_label_set_text(s_clock, buf);
}

void ui_dirty(uint32_t bits)
{
    s_dirty |= bits;
}

/* ------------------------------------------------------------ UI task */

static void apply(const ui_evt_t *evt)
{
    switch (evt->type) {
    case EVT_LINK:
        lv_obj_set_style_text_color(
            s_link_dot, lv_color_hex(evt->flag ? UI_C_OK : UI_C_BAD), 0);
        ui_log(UI_LOG_NOTE, evt->text != NULL ? evt->text : "");
        break;
    case EVT_WIFI:
        ui_status_set_wifi(evt->text, evt->num);
        break;
    case EVT_IP:
        ui_status_set_ip(evt->text);
        break;
    case EVT_ACCOUNT:
        ui_status_set_account(evt->text);
        break;
    case EVT_GOTO:
        nav_goto((ui_page_id_t)evt->slot);
        break;
    case EVT_BACK:
        nav_back();
        break;
    case EVT_CALL_ENDED:
        ui_call_set_end_reason(evt->text);
        break;
    default:
        break;
    }
}

static void ui_task(void *arg)
{
    (void)arg;

    TickType_t last = xTaskGetTickCount();

    while (true) {
        ui_evt_t evt   = { 0 };
        bool     got   = xQueueReceive(s_queue, &evt, pdMS_TO_TICKS(UI_TICK_MS)) == pdTRUE;
        uint32_t dirty = s_dirty;

        s_dirty = 0;

        /* A second has passed, so the clock moved; the status page also has
         * an uptime and a heap reading that nothing else pushes. */
        if (xTaskGetTickCount() - last >= pdMS_TO_TICKS(1000)) {
            last = xTaskGetTickCount();
            dirty |= UI_DIRTY_STATUS;
        }

        if (!got && dirty == 0) {
            continue;
        }

        if (!bsp_display_lock(1000)) {
            ESP_LOGW(TAG, "display busy, dropped an update");
            free(evt.text);
            continue;
        }

        if (got) {
            apply(&evt);
        }
        if (dirty != 0) {
            if (dirty & UI_DIRTY_STATUS) {
                tick_clock();
            }
            s_def->refresh(dirty);
            apply_title();
        }
        bsp_display_unlock();
        free(evt.text);

        /* Outside the lock on purpose: this is where a page asks the server
         * for what it found missing, and asking blocks on the long link's
         * socket. Holding the display lock across that freezes LVGL --
         * repaints and touch both -- which reads as a dead board rather than
         * a slow one. Nothing switches pages in between, because navigation
         * also happens on this task. */
        if (s_def->prime != NULL) {
            s_def->prime();
        }
    }
}

static void post(uint8_t type, uint8_t slot, bool flag, int32_t num,
                 const char *text)
{
    if (s_queue == NULL) {
        return;
    }

    ui_evt_t evt = { .type = type, .slot = slot, .flag = flag, .num = num,
                     .text = text != NULL ? strdup(text) : NULL };

    if (text != NULL && evt.text == NULL) {
        return;
    }
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "UI queue full, dropped an update");
        free(evt.text);
    }
}

/* ----------------------------------------------------- the client's events */

/* All of these run on the wfc task. They set bits and append log lines; not
 * one of them touches a widget. */

static void on_connection_status(int status, void *ud)
{
    (void)ud;

    static const struct { int status; const char *text; bool ok; } TEXTS[] = {
        { WFC_STATUS_CONNECTING,      "连接服务器",       false },
        { WFC_STATUS_CONNECTED,       "已连接",           true  },
        { WFC_STATUS_RECEIVING,       "同步中",           true  },
        { WFC_STATUS_KICKED_OFF,      "被踢下线",         false },
        { WFC_STATUS_TOKEN_INCORRECT, "token 不对",       false },
        { WFC_STATUS_SERVER_DOWN,     "服务器连不上",     false },
        { WFC_STATUS_REJECTED,        "服务器拒绝了连接", false },
        { WFC_STATUS_UNCONNECTED,     "已断开",           false },
    };

    for (size_t i = 0; i < sizeof(TEXTS) / sizeof(TEXTS[0]); i++) {
        if (TEXTS[i].status == status) {
            ui_set_link_state(TEXTS[i].text, TEXTS[i].ok);
            ui_dirty(UI_DIRTY_STATUS);
            return;
        }
    }
    ui_set_link_state(wfc_status_str((wfc_connection_status_t)status), false);
    ui_dirty(UI_DIRTY_STATUS);
}

static void on_receive_messages(const wfc_message_t *msgs, size_t n, bool has_more,
                                void *ud)
{
    (void)has_more;
    (void)ud;

    for (size_t i = 0; i < n; i++) {
        const wfc_message_t *msg   = &msgs[i];
        bool                 mine  = msg->direction == WFC_DIRECTION_SEND;
        bool                 group = msg->conversation.type == WFC_CONV_GROUP;
        char                 who[WFC_NAME_MAX];
        char                 digest[WFC_DIGEST_MAX];

        wfc_get_display_name(mine ? msg->conversation.target : msg->from,
                             group ? msg->conversation.target : NULL, who,
                             sizeof(who));
        wfc_message_digest(msg, digest, sizeof(digest));

        /* The panel is the real output, but a board on a bench usually has a
         * serial cable and nobody looking at the screen. */
        ESP_LOGI(TAG, "%s %s%s: %s", mine ? "sent" : "recv", group ? "group/" : "",
                 who, digest);
        ui_logf(mine ? UI_LOG_OUT : UI_LOG_IN, "%s%s\n%s", mine ? "→ " : "← ",
                who, digest);
    }

    ui_dirty(UI_DIRTY_CONVS | UI_DIRTY_MESSAGES | UI_DIRTY_STATUS);
}

static void on_send_result(int error_code, int64_t message_uid, int64_t timestamp,
                           void *ud)
{
    (void)timestamp;
    (void)ud;

    if (error_code == 0) {
        ESP_LOGI(TAG, "sent, uid %lld", (long long)message_uid);
    } else {
        ui_logf(UI_LOG_ERROR, "发送失败，错误码 %d", error_code);
    }
    ui_dirty(UI_DIRTY_CONVS | UI_DIRTY_MESSAGES | UI_DIRTY_STATUS);
}

static void on_recall(const char *operator_uid, int64_t message_uid, void *ud)
{
    (void)message_uid;
    (void)ud;

    char who[WFC_NAME_MAX];

    wfc_get_display_name(operator_uid, NULL, who, sizeof(who));
    ui_logf(UI_LOG_NOTE, "%s 撤回了一条消息", who);
    ui_dirty(UI_DIRTY_CONVS | UI_DIRTY_MESSAGES);
}

static void on_conversation_update(const wfc_conversation_info_t *info, void *ud)
{
    (void)info;
    (void)ud;
    ui_dirty(UI_DIRTY_CONVS | UI_DIRTY_STATUS);
}

/* A profile landing is what turns <uEPhwEwgg> into a name. Every page that
 * draws a name has to be told, and none of them has to know which name. */
static void on_user_infos(const wfc_user_info_t *users, size_t n, void *ud)
{
    (void)users;
    (void)n;
    (void)ud;
    ui_dirty(UI_DIRTY_NAMES);
}

static void on_group_infos(const wfc_group_info_t *groups, size_t n, void *ud)
{
    (void)groups;
    (void)n;
    (void)ud;
    ui_dirty(UI_DIRTY_NAMES);
}

static void on_group_members(const char *group_id, size_t n, void *ud)
{
    (void)ud;
    ESP_LOGI(TAG, "group %s: %u member(s) cached", group_id, (unsigned)n);
    ui_dirty(UI_DIRTY_NAMES);
}

static void on_friends(size_t n, void *ud)
{
    (void)ud;
    ui_logf(UI_LOG_NOTE, "好友列表更新 %u 条", (unsigned)n);
    /* Both bits: the list gained or lost a row, and an alias that changed is
     * a name every page that draws one has to redraw. */
    ui_dirty(UI_DIRTY_FRIENDS | UI_DIRTY_NAMES);
}

static void subscribe_all(void)
{
    wfc_on_connection_status(on_connection_status, NULL);
    wfc_on_receive_messages(on_receive_messages, NULL);
    wfc_on_send_result(on_send_result, NULL);
    wfc_on_recall_message(on_recall, NULL);
    wfc_on_conversation_update(on_conversation_update, NULL);
    wfc_on_user_infos_update(on_user_infos, NULL);
    wfc_on_group_infos_update(on_group_infos, NULL);
    wfc_on_group_members_update(on_group_members, NULL);
    wfc_on_friend_list_update(on_friends, NULL);

    /* The call page subscribes itself, because its events are the only ones
     * whose types come from the AV SDK -- and the AV SDK is optional
     * (ui_page.h). In a build without it this is the stub and does nothing. */
    ui_call_subscribe();
}

/* ------------------------------------------------------------ public API */

void ui_init(void)
{
    s_queue = xQueueCreate(24, sizeof(ui_evt_t));
    assert(s_queue != NULL);

    lv_display_t *disp = bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(0);

    /* The font is the whole reason this call is here rather than the default
     * theme: LVGL's built-in CJK font is a subset that cannot draw 会话 or
     * 状态, and installing ours theme-wide is what makes every widget --
     * including the keyboard's own labels -- inherit it. */
    /* Cheap: the fetcher task itself is not created until a picture is
     * actually looked at (ui_media.h). */
    ui_media_start();

    lv_theme_t *theme = lv_theme_default_init(
        disp, lv_color_hex(UI_C_ACCENT), lv_color_hex(UI_C_DIM), true, &wfc_font_16);
    lv_display_set_theme(disp, theme);

    lv_obj_t *scr = lv_screen_active();
    ui_style_flat(scr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    build_header(scr);

    s_content = lv_obj_create(scr);
    ui_style_flat(s_content);
    lv_obj_set_width(s_content, LV_PCT(100));
    lv_obj_set_flex_grow(s_content, 1);

    build_navbar(scr);

    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);

    s_def = PAGES[UI_PAGE_CONVS];
    s_def->create(s_content);
    apply_chrome();
    s_def->refresh(UI_DIRTY_ALL);

    bsp_display_unlock();

    /* Its own task rather than an LVGL timer, because a repaint reads the
     * store and a store read can be a SQLite query on FATFS. That belongs on
     * a stack we control, not on LVGL's rendering task. */
    xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL);

    subscribe_all();
}

void ui_client_ready(void)
{
    ui_dirty(UI_DIRTY_ALL);
}

/* The one thing a page is told rather than reads, and the reason is in
 * ui_page.h: a call's ending phrase is only in the event. This is the AV-task
 * side -- a post like any other -- and EVT_CALL_ENDED above is the UI-task
 * side that delivers it. */
void ui_call_post_end_reason(const char *why)
{
    post(EVT_CALL_ENDED, 0, false, 0, why);
}

void ui_set_link_state(const char *text, bool ok)
{
    post(EVT_LINK, 0, ok, 0, text);
}

void ui_set_wifi(const char *ssid, int rssi)
{
    post(EVT_WIFI, 0, false, rssi, ssid);
}

void ui_set_ip(const char *ip)
{
    post(EVT_IP, 0, false, 0, ip);
}

void ui_set_account(const char *user_id)
{
    post(EVT_ACCOUNT, 0, false, 0, user_id);
}

void ui_log(ui_log_kind_t kind, const char *text)
{
    char      stamp[16];
    char      line[UI_TEXT_MAX + 24];
    time_t    now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    snprintf(line, sizeof(line), "%s  %.*s", stamp, UI_TEXT_MAX, text);
    ui_log_append(kind, line);
}

void ui_logf(ui_log_kind_t kind, const char *fmt, ...)
{
    char    buf[UI_TEXT_MAX];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ui_log(kind, buf);
}
