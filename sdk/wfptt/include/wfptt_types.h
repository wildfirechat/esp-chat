/* 基本类型：四种消息类型的编号、请求发言可能遇到的错误，以及一次发言的结束
 * 原因。
 *
 * 这里的每个数字都来自各家参考客户端，都不能重新编号 —— 它们会在本设备和手机
 * 之间的协议里传递。
 *
 * 本组件不含任何中文文案，理由与 wfc-esp 的消息类型表相同：给人看的文字属于
 * 应用。下面的 *_str() 函数返回的是给日志看的 ASCII 标签。
 */

#ifndef WFPTT_TYPES_H
#define WFPTT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 消息类型 */

/* 音频本身：MessageContent.data 里的裸 AMR-NB 帧，没有文件头，每
 * WFPTT_CHUNK_MS 一条消息。它是透传的 —— 哪里都不存、不计入任何人的未读数，
 * 正因如此才可能一秒发两三条。 */
#define WFPTT_CONTENT_SOUND_DATA 21

/* “我说完了”。透传，不带任何内容。 */
#define WFPTT_CONTENT_END        22

/* 可选的留底：把整次按住说话的录音，作为一条普通语音消息上传，好让会话里留下
 * 记录。四种类型里只有它会被存储，而且除了类型号之外它就是一条语音消息 ——
 * remoteMediaUrl、mediaType 2、{"duration":N} —— 所以从没听说过对讲功能的客户端
 * 照样能播放它。 */
#define WFPTT_CONTENT_SOUND      23

/* “我要开始说话了”，带上说话人的优先级。透传。
 *
 * 它不是听到别人说话的必要条件：没收到它的一方会在第一条音频消息到达时开始播放
 * （参考客户端也是这么做的）。它买到的是第一块音频到达之前的那半秒 —— 界面可以
 * 提前显示出是谁在说话。 */
#define WFPTT_CONTENT_START      24

/* -------------------------------------------------------------------- 状态 */

typedef enum {
    WFPTT_IDLE = 0,
    /* 已申请频道，正在等服务器裁定谁拿到它。只有在只允许一个人说话的频道里才会
     * 出现，因为只有那种情况才有锁要等。 */
    WFPTT_REQUESTING,
    WFPTT_TALKING,
} wfptt_state_t;

/* -------------------------------------------------------------------- 错误 */

/* 发言请求被拒绝的原因。取负值是有理由的：同一个字段里的正数是服务器的应答码，
 * 这样两者可以放在一起传而不会混淆。 */
#define WFPTT_ERR_UNKNOWN           (-1)
/* 频道在别人手上。这是锁给出的答复，在两人频道里就是普通的“对方正在说话”。 */
#define WFPTT_ERR_OCCUPIED          (-2)
/* 正在说话的人数已经达到该频道允许的上限。 */
#define WFPTT_ERR_MAX_SPEAKER       (-3)
#define WFPTT_ERR_GROUP_MUTED       (-4)
#define WFPTT_ERR_GROUP_MEMBER_MUTED (-5)
/* 本机已经在说话了。 */
#define WFPTT_ERR_TALKING           (-6)
#define WFPTT_ERR_NOT_IN_GROUP      (-7)
#define WFPTT_ERR_PTT_DISABLED      (-8)
/* 麦克风打不开，或者音频通道被别的功能占着 —— 一通电话，或者正在录制、播放的
 * 语音消息。 */
#define WFPTT_ERR_RECORDER_ERROR    (-9)
/* 长连接断开。参考客户端里没有这一项，因为轮到它们工作时总是在线的；而放在架子
 * 上的设备可能不在线，“请求根本没发出去”和“频道在别人手上”是两回事。 */
#define WFPTT_ERR_DISCONNECTED      (-10)

/* ---------------------------------------------------------------- 结束原因 */

/* 一次发言以哪种方式结束，界面事后要说的话全在这里。 */
typedef enum {
    WFPTT_END_USER_RELEASE = 0,  /* 松开了按钮 */
    WFPTT_END_TIMEOUT      = 1,  /* 达到单次发言时长上限 */
    WFPTT_END_TAKE_OVER    = 2,
    WFPTT_END_NETWORK      = 3,
    WFPTT_END_CHANNEL_MUTED = 4,
    WFPTT_END_MEMBER_MUTED = 5,
    WFPTT_END_MEDIA        = 6,  /* 麦克风停止工作 */
    WFPTT_END_NOT_IN_CHANNEL = 7,
    WFPTT_END_USER_DISABLED = 8,
} wfptt_end_reason_t;

/* -------------------------------------------------------------------- 名字 */

/* 给日志用。不会为 NULL。 */
const char *wfptt_state_str(wfptt_state_t state);
const char *wfptt_error_str(int error_code);
const char *wfptt_end_reason_str(wfptt_end_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* WFPTT_TYPES_H */
