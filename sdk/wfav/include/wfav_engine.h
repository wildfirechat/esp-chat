/* 引擎：同一时刻只有一通电话，以及发起或接听一通电话所需的一切。
 *
 * 它负责路由 —— 从 IM 消息流里挑出通话消息、转成通话状态 —— 也持有那个唯一的
 * 当前会话，因为一块只有一个扬声器和一个麦克风的设备，同时只可能在一通电话里。
 *
 * ------------------------------------------------------------------------
 * 它是怎么挂在 IM 客户端上的。
 *
 * 没有第二条连接，也没有第二套协议。WFC 把通话控制当作普通 IM 消息传递
 * （类型 400..417，见 wfc_model.h），把 Janus 房间协议放在同一条长连接的房间
 * 主题上。所以引擎订阅客户端的两个事件、调用客户端的两个接口：
 *
 *   收   wfc_on_receive_messages()      邀请、接听、挂断
 *   收   wfc_on_conference_event()      服务器推送的房间事件
 *   发   wfc_send_message()             邀请、接听、挂断
 *   发   wfc_send_conference_request()  建房间、加入、发布、离开
 *
 * 接缝就这么多。长连接断开时电话打不成，引擎会如实报告，而不是把它排进队列。
 *
 * ------------------------------------------------------------------------
 * 本版本做什么、不做什么。
 *
 * 做：接听 1v1 语音来电、发起语音通话、挂断、静音，以及报告房间里有谁。
 * 16 kHz 单声道 Opus，采集侧开回声消除。
 *
 * 不做：任何方向的视频（设备没有摄像头，也解不出可用的帧率）、有主持人和观众的
 * 会议、屏幕共享、远程控制，以及在通话中再邀请第三个人。群通话是明确拒绝而不是
 * 接一半：收到群通话邀请会直接回一条“忙”的挂断消息，免得群里一直等着一个永远
 * 不会发布媒体的参与者。
 */

#ifndef WFAV_ENGINE_H
#define WFAV_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfav_event.h"
#include "wfav_session.h"
#include "wfav_types.h"
#include "wfc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一台 TURN/STUN 服务器。WFC 不像某些部署那样在连接时下发它们 —— 它们属于客户端
 * 配置，这里从 Kconfig 读取 —— 所以自建了 TURN 的部署只需要改 sdkconfig，其他
 * 什么都不用动。 */
typedef struct {
    const char *url;        /* "turn:turn.example.com:3478" 或 "stun:..." */
    const char *username;
    const char *password;
} wfav_ice_server_t;

#define WFAV_MAX_ICE_SERVERS 2

typedef struct {
    wfav_ice_server_t ice_servers[WFAV_MAX_ICE_SERVERS];
    size_t            n_ice_servers;

    /* 只使用中继候选地址。默认关闭。
     *
     * 强制中继并不会减少失败的可能，它只是去掉了那些本来能用的连接路径，让整通
     * 电话完全依赖 TURN 服务器。只有在 Janus 确实除了 TURN 之外无法到达的部署
     * 里，才应该打开它。 */
    bool force_relay;

    /* 无人接听时响铃多少秒后放弃，两端都适用。0 表示 60，与其他 WFC 客户端一致；
     * 只改一端会导致那一端先挂断，另一端把它报告成对方挂断而不是超时。 */
    int ring_timeout_s;
} wfav_engine_config_t;

/* 用内置默认值填充 cfg：来自 Kconfig 的 ICE 服务器、不强制中继（见上面的
 * force_relay）、60 秒响铃。先调用它，再改需要改的项，然后传下去。 */
void wfav_engine_default_config(wfav_engine_config_t *cfg);

/* 启动音视频任务并订阅 IM 客户端的事件。
 *
 * 请在 wfc_client_init() 之后、wfc_client_connect() 之前调用，这样连接建立后
 * 最初几秒里到达的来电邀请才不会漏掉。它不会碰音频硬件：编解码设备在通话开始时
 * 打开、结束时关闭，因为一直开着 I2S 通道意味着只要设备开着，内部 SRAM 里就一直
 * 占着 DMA 缓冲。 */
esp_err_t wfav_engine_start(const wfav_engine_config_t *cfg);

/* 挂断进行中的通话并停止任务。不要在事件回调里调用 —— 它要等待回调所在的那个
 * 任务退出。 */
void wfav_engine_stop(void);

/* -------------------------------------------------------------- 发起通话 */

/* 呼叫 user_id，仅音频。
 *
 * 立即返回：建房间和发邀请都在音视频任务上进行，无论成败，稍后都会通过
 * wfav_on_call_started() 通知。已经在通话中时返回 ESP_ERR_INVALID_STATE；长连接
 * 断开时返回 ESP_ERR_NOT_SUPPORTED。
 *
 * 任意任务均可调用，包括 UI 任务 —— 这一点很重要，因为触发它的那个按钮运行在
 * 持有显示锁的 LVGL 任务上。 */
esp_err_t wfav_start_call(const char *user_id);

/* 只要有一通不处于 IDLE 状态的电话就返回 true。会话界面用它来判断通话按钮要不
 * 要可用，开销很小。 */
bool wfav_is_busy(void);

/* ---------------------------------------------------------------- 扬声器 */

/* 扬声器音量，0..100。它不属于通话协议，也不属于某一通电话 —— 它是编解码设备的
 * 输出音量，调低之后一直是低的 —— 但它属于通话界面，所以放在这里，免得界面代码
 * 去直接操作 esp_codec_dev。 */
esp_err_t wfav_set_speaker_volume(int percent);
int       wfav_get_speaker_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* WFAV_ENGINE_H */
