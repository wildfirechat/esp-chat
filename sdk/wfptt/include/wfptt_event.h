/* 强类型的对讲事件，约定与 wfc_event.h、wfav_event.h 相同。
 *
 * 五个事件，是参考客户端那两个回调展开后的结果：一个是本机按下按钮之后发生的事
 * （开始 / 结束 / 被拒绝），另一个是别人按下按钮之后发生的事（开始说话 / 停止
 * 说话）。每个事件一个回调类型、一个订阅函数，回调保存在联合体里而不是藏在强制
 * 类型转换后面 —— 理由与 wfc_event.h 相同：某个事件多了一个参数时，应该得到一串
 * 编译错误。
 *
 * ------------------------------------------------------------------------
 * 线程模型。
 *
 * 所有回调都在 wfptt 任务上执行。不是长连接的任务，也不是播放任务：IM 消息到达、
 * 锁的应答到达、发言计时器到期，是三个任务带来的输入，把它们汇聚到一个任务上，
 * 状态机才是单线程的。
 *
 * 于是又是那三条规则：
 *
 *   - 不要阻塞。这个任务同时还要每 400 ms 发布一块音频，晚发出去的一块，对端
 *     听到的就是一段空白。
 *   - 不要碰 LVGL，请投递给 UI 任务。
 *   - 不要调用 wfptt_stop() 之后再等待什么。wfptt_release_talk() 是安全的，也是
 *     正常的做法。
 *
 * 订阅和退订在任意任务里都安全，在被退订的那个回调内部也安全。
 *
 * 传给回调的所有指针随该次调用失效。
 */

#ifndef WFPTT_EVENT_H
#define WFPTT_EVENT_H

#include <stdbool.h>
#include <stddef.h>

#include "wfc_model.h"
#include "wfptt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wfptt_subscription wfptt_subscription_t;

/* -------------------------------------------------------------------- 事件 */

/* 频道拿到了：麦克风已经打开，第一块音频正在路上。这是显示“正在说话”提示的时机，
 * 硬件上有提示音的话，也是该响的时机。
 *
 * 在只允许一个人说话的频道上，它发生在按钮按下之后的一次往返之后 —— 锁需要去
 * 申请 —— 所以它是一个事件，而不是一个返回值。 */
typedef void (*wfptt_on_talk_begin_t)(const wfc_conversation_t *conv, void *ud);

/* 本机的发言结束了，原因如参数所示：松开了按钮、达到时长上限、被群禁言、连接
 * 断开等。这个回调执行时麦克风已经关闭。 */
typedef void (*wfptt_on_talk_end_t)(const wfc_conversation_t *conv,
                                    wfptt_end_reason_t reason, void *ud);

/* 频道申请被拒绝。error_code 是 WFPTT_ERR_* 之一，其中值得明确告诉用户的是
 * OCCUPIED：麦克风在别人手上。此时什么都没有打开，也什么都没有发出去。 */
typedef void (*wfptt_on_talk_failed_t)(const wfc_conversation_t *conv,
                                       int error_code, void *ud);

/* 有人开始说话，或者停止了说话。
 *
 * “停止”有两种来源：对方的结束消息，或者对方的沉默 —— 超过
 * WFPTT_TALKER_TIMEOUT_MS 没有收到音频的说话人会被当作已经离开，因为说到一半被
 * 关机的客户端不会发出结束通知，否则频道会一直被占着。
 *
 * 被告知有人在说话，和听得见他，不是一回事。本设备只有一个扬声器、没有混音器，
 * 所以一次只播一个人（见 wfptt_client.h）；其余的人仍然会在这里报出来，界面因此
 * 可以显示一个有三个人的频道。 */
typedef void (*wfptt_on_user_start_talking_t)(const wfc_conversation_t *conv,
                                              const char *user_id, void *ud);
typedef void (*wfptt_on_user_end_talking_t)(const wfc_conversation_t *conv,
                                            const char *user_id, void *ud);

/* -------------------------------------------------------------------- 订阅 */

wfptt_subscription_t *wfptt_on_talk_begin(wfptt_on_talk_begin_t cb, void *ud);
wfptt_subscription_t *wfptt_on_talk_end(wfptt_on_talk_end_t cb, void *ud);
wfptt_subscription_t *wfptt_on_talk_failed(wfptt_on_talk_failed_t cb, void *ud);
wfptt_subscription_t *wfptt_on_user_start_talking(wfptt_on_user_start_talking_t cb,
                                                  void *ud);
wfptt_subscription_t *wfptt_on_user_end_talking(wfptt_on_user_end_talking_t cb,
                                                void *ud);

/* 传 NULL 是安全的，在被退订的那个回调内部调用也是安全的。 */
void wfptt_unsubscribe(wfptt_subscription_t *sub);

/* 逐个退订并把数组元素置空 —— 一行拆掉一个页面的所有订阅。 */
void wfptt_unsubscribe_all(wfptt_subscription_t **subs, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* WFPTT_EVENT_H */
