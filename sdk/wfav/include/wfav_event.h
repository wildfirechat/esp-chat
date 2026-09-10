/* 强类型的通话事件，约定与 wfc_event.h 相同。
 *
 * 每个事件一个回调类型、一个订阅函数，回调保存在联合体里而不是藏在强制类型转换
 * 后面，理由和 wfc_event.h 一样：某个事件多了一个参数时，应该得到一串编译错误，
 * 而不是一块正在读一个已经不存在的结构的板子。
 *
 * ------------------------------------------------------------------------
 * 线程模型，以及它与 wfc_event.h 不同的地方。
 *
 * 所有回调都在音视频任务上执行，也就是 wfav 自己的任务，而不是长连接的任务。
 * 这是有意的：通话状态机会被三处不同的输入驱动（长连接任务上到达的 IM 消息、
 * 同一个任务上到达的 Janus 应答、esp_peer 任务上到达的连接状态变化），把三者
 * 汇聚到一个任务上，状态机才是单线程的、可读的。
 *
 * 随之而来的规则与 wfc_event.h 一致：
 *
 *   - 不要在回调里阻塞。音视频任务同时也负责回应 Janus 的心跳，心跳停了房间
 *     会把这通电话踢掉。
 *   - 不要碰 LVGL，请投递给 UI 任务，或用 lv_async_call。
 *   - 不要调用 wfav_engine_stop() 或 wfav_hangup() 之后再等待什么；挂断本身是
 *     安全的，也是正常的做法。
 *
 * 订阅和退订在任意任务里都安全，在回调内部也安全，包括退订当前正在执行的这个
 * 订阅。
 *
 * 生命周期：传给回调的所有指针随该次调用失效。要留下的用户 ID 请复制。
 */

#ifndef WFAV_EVENT_H
#define WFAV_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wfav_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wfav_subscription wfav_subscription_t;

/* -------------------------------------------------------------------- 事件 */

/* 出现了一通电话。incoming 为 false 表示是本机发起的。
 *
 * 每通电话触发一次，且在任何状态变化之前触发，是“把通话界面弹出来”的时机。通话
 * 的其他信息不随事件传出，而是通过 wfav_session.h 读回来，这样界面因为别的原因
 * 重画时读到的值，与这个事件本来会携带的值是一致的。 */
typedef void (*wfav_on_call_started_t)(const char *call_id, bool incoming,
                                       void *ud);

/* 状态发生了变化，取值见 wfav_call_state_t。
 *
 * WFAV_STATE_IDLE 不会通过这个事件报出来 —— 通话结束是下面单独的事件，因为在
 * IDLE 时就关掉界面，会让它来不及说明结束的原因。 */
typedef void (*wfav_on_state_changed_t)(wfav_call_state_t state, void *ud);

/* 通话结束了，原因如参数所示；这个回调返回时会话已经不存在。duration_ms 是媒体
 * 实际流动的时长 —— 从未接通的通话是 0，未接来电因此能与一通很短的通话区分
 * 开来。 */
typedef void (*wfav_on_call_ended_t)(wfav_end_reason_t reason,
                                     int64_t duration_ms, void *ud);

/* 有人加入了房间、有人的媒体接通了、有人离开了。最后一个的 reason 是“他”为什么
 * 离开，这未必就是整通电话结束的原因。 */
typedef void (*wfav_on_participant_joined_t)(const char *user_id, void *ud);
typedef void (*wfav_on_participant_connected_t)(const char *user_id, void *ud);
typedef void (*wfav_on_participant_left_t)(const char *user_id,
                                           wfav_end_reason_t reason, void *ud);

/* 有人的麦克风被静音或取消静音 —— 包括本机自己，界面因此可以据此确认自己的静音
 * 按钮真的生效了，而不是假定它生效了。 */
typedef void (*wfav_on_mute_changed_t)(const char *user_id, bool audio_muted,
                                       void *ud);

/* 错误里的 code 属于哪个编号空间。
 *
 * 有三套编号会通过这个回调以同一个 int 传出来：结束原因、Janus videoroom 插件
 * 的错误码、以及传输层的应答码。界面拿到一个 4 无法分辨那是“媒体错误”还是
 * “房间错误 4”，于是只能把 detail 原样打出来。加上 domain，这个数字才重新有了
 * 意义。 */
typedef enum {
    WFAV_ERR_MEDIA = 0,  /* code 是 wfav_end_reason_t */
    WFAV_ERR_ROOM,       /* code 是 videoroom 的错误码，或传输层的应答码 */
} wfav_error_domain_t;

/* 出了问题，但通话没有因此结束：某个 Janus 请求被拒绝、某路订阅建立不起来等。
 * detail 是给日志看的一段简短英文，随该次调用失效。 */
typedef void (*wfav_on_error_t)(wfav_error_domain_t domain, int code,
                                const char *detail, void *ud);

/* -------------------------------------------------------------------- 订阅 */

wfav_subscription_t *wfav_on_call_started(wfav_on_call_started_t cb, void *ud);
wfav_subscription_t *wfav_on_state_changed(wfav_on_state_changed_t cb, void *ud);
wfav_subscription_t *wfav_on_call_ended(wfav_on_call_ended_t cb, void *ud);
wfav_subscription_t *wfav_on_participant_joined(wfav_on_participant_joined_t cb,
                                                void *ud);
wfav_subscription_t *wfav_on_participant_connected(
    wfav_on_participant_connected_t cb, void *ud);
wfav_subscription_t *wfav_on_participant_left(wfav_on_participant_left_t cb,
                                              void *ud);
wfav_subscription_t *wfav_on_mute_changed(wfav_on_mute_changed_t cb, void *ud);
wfav_subscription_t *wfav_on_error(wfav_on_error_t cb, void *ud);

/* 传 NULL 是安全的，在被退订的那个回调内部调用也是安全的。退订之后句柄即失效。 */
void wfav_unsubscribe(wfav_subscription_t *sub);

/* 逐个退订并把数组元素置空 —— 一行拆掉一个页面的所有订阅。 */
void wfav_unsubscribe_all(wfav_subscription_t **subs, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* WFAV_EVENT_H */
