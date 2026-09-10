/* 一通电话，从外部看是什么样。
 *
 * 通话界面靠这里的接口把自己画出来，界面上的按钮调用的也是这里。所有接口在任意
 * 任务里都可以调用 —— 读接口拿一下短锁再复制出来，写接口把请求投递给音视频任务
 * 后立即返回 —— 所以持有显示锁的 LVGL 事件回调可以随意使用它们。
 *
 * ------------------------------------------------------------------------
 * 这里没有会话句柄，是有意的。
 *
 * 设备只有一个麦克风和一个扬声器，所以它要么正好在一通电话里，要么一通都没有。
 * 一个接收 wfav_session_t * 却完全不看它的 API，描述的是一个并不存在的系统，
 * 而且会迫使每个调用方跨重画持有一个指针，而通话结束随时可能让它失效。
 *
 * 所以通话是隐式的，每个读接口回答的都是“当前那一通”。没有通话时，每个接口返回
 * 各自的空值：IDLE、""、0、没有参与者。通话结束后又画了一帧的界面画出来的是一个
 * 空的通话界面，而不是崩溃，然后在下一帧关掉自己。
 */

#ifndef WFAV_SESSION_H
#define WFAV_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfav_types.h"
#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- 读 */

wfav_call_state_t wfav_session_state(void);

/* 通话 ID、通话所属的会话，以及发起人。每条信令消息都以通话 ID 为键，所以按
 * 通话打日志的界面记的就是它。不会为 NULL；没有通话时是 ""。 */
const char *wfav_session_call_id(void);
const char *wfav_session_initiator(void);
void        wfav_session_conversation(wfc_conversation_t *out);

/* 这通电话是否由本机拨出。 */
bool wfav_session_is_outgoing(void);

/* 麦克风当前是否静音。 */
bool wfav_session_audio_muted(void);

/* 媒体已经流动了多少毫秒，尚未开始时为 0。这就是通话界面上那个计时器显示的值。
 * 它从第一路 PeerConnection 连通的时刻算起，而不是从接听算起：花了四秒才配对
 * 成功的通话，不该声称已经通话了四秒。 */
int64_t wfav_session_duration_ms(void);

/* 把通话中的其他人复制进 out，返回实际写入的个数，最多 max 个。
 *
 * 1v1 通话时这是一条记录，也就是“我在和谁通话”的答案。这里做复制，是因为另一种
 * 做法是在整个重画期间持有会话锁，而重画恰恰是音视频任务想拿这把锁去处理"有人
 * 离开"的时候。 */
size_t wfav_session_participants(wfav_participant_t *out, size_t max);

/* 对端那一个人，适用于本版本唯一支持的 1v1 场景。通话还没有参与者时写入 ""
 * 并返回 false。 */
bool wfav_session_peer(char *buf, size_t buf_size);

/* -------------------------------------------------------------------- 写 */

/* 接听来电。状态不是 INCOMING 时是空操作，所以接听按钮被连点两下也无害。
 *
 * 请求排入队列后即返回。接听消息发出、状态变为 CONNECTING、房间相关的工作都在
 * 音视频任务上进行。 */
esp_err_t wfav_answer(void);

/* 结束通话：响铃中是拒接，已接通是挂断。两者是同一条消息、不同的原因，而原因由
 * 状态决定，所以界面上一个按钮就够了。 */
esp_err_t wfav_hangup(void);

/* 静音或取消静音麦克风。本地立即生效，并会通知房间，好让对端界面显示出来。 */
esp_err_t wfav_set_audio_muted(bool muted);

#ifdef __cplusplus
}
#endif

#endif /* WFAV_SESSION_H */
