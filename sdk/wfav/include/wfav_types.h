/* 通话的基本类型：通话有哪些状态、可能怎样结束、房间会返回哪些错误。
 *
 * 这里每个取值都由协议规定，不是本实现决定的 —— 它们会在 ESP32 和手机之间的
 * 挂断消息里传递，在这里自己编一个号，对端读到的就是别的意思。
 *
 * ------------------------------------------------------------------------
 * 只做音频。
 *
 * ESP32-S3-BOX-3B 没有任何摄像头接口，所以没有视频可发；而在 S3 上用软件解
 * H.264，同时还要跑 Opus、回声消除、WiFi 和 LVGL，上限大约是 320x240、10 fps。
 * 因此本 SDK 只协商一路音频，从不提供视频：它发起和接听的每一通电话，
 * audio_only 都是 true。
 *
 * 协议里与视频有关的部分仍然要理解，因为对端会发过来：手机发起视频通话时
 * audio_only 为 false，本 SDK 会以音频接听（这在各家客户端里都叫“降级”，都
 * 支持）。缺的是产生和显示视频的能力，而不是与有视频的一端通话的能力。
 */

#ifndef WFAV_TYPES_H
#define WFAV_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 尺寸 */

/* 通话 ID 是主叫生成的 UUID，各客户端用的是 36 个字符再加上部署方的前缀。 */
#define WFAV_CALL_ID_MAX 64

/* 房间 PIN：六位数字，由建房间的一方生成，之后每个针对该房间的请求都要带上。 */
#define WFAV_PIN_MAX 16

/* 一通电话里最多跟踪多少个其他参与者。
 *
 * 协议允许纯音频通话有 16 个参与者，建房间时也是按这个数申请的，好让后面加入的
 * 手机不被挡在外面。这里这个数字是另一回事：设备自己跟踪多少个。
 *
 * 本版本里实际只会用到其中一个槽位。群通话的邀请在建立会话之前就被拒绝了，所以
 * 一通电话就是一个远端参与者、一路发布、一路订阅 —— 这也是 esp_peer 在 S3 上
 * 给出的数字：同时两个 peer。每多一个参与者，就要多一个带独立 DTLS 会话和抖动
 * 缓冲的 PeerConnection，而第三个所需的内部 SRAM 是没有的。
 *
 * 所以 4 是给簿记留的余量，不是支持的参与者数量：房间事件里可能出现比 1v1 通话
 * 更多的流，它们就落在这个数组里。要真正支持群通话，先要解决内存问题，而不只是
 * 改大这个数字。 */
#define WFAV_MAX_PARTICIPANTS 4

/* -------------------------------------------------------------- 通话状态 */

/* 一通电话在任一时刻只处于其中一个状态，界面完全由它决定：
 *
 *   IDLE        没有通话，或上一通已经结束
 *   OUTGOING    我方拨出，等待对方接听    （对方在响铃）
 *   INCOMING    对方拨入，我方尚未接听    （本机在响铃）
 *   CONNECTING  双方都已接听，正在建房间和媒体通道
 *   CONNECTED   媒体已经流动
 *
 * 从 INCOMING 或 OUTGOING 迈入 CONNECTING 才是 Janus 那一侧开始工作的时刻：
 * 加入房间和发布媒体都在那时进行，而不是在接听时 —— 因为主叫是在收到被叫的
 * 接听消息时才进入 CONNECTING 的。 */
typedef enum {
    WFAV_STATE_IDLE       = 0,
    WFAV_STATE_OUTGOING   = 1,
    WFAV_STATE_INCOMING   = 2,
    WFAV_STATE_CONNECTING = 3,
    WFAV_STATE_CONNECTED  = 4,
} wfav_call_state_t;

const char *wfav_call_state_str(wfav_call_state_t state);

/* -------------------------------------------------------------- 结束原因 */

/* 结束原因会在网络上传递：挂断消息里带着一个，接收方要把它映射到自己的视角 ——
 * 对方的“我挂断”就是我方的“对方挂断”。
 *
 * 要分清的是本地与远端。不带 Remote 的是我方做的或发生在我方的事；带 Remote 的
 * 是同一件事发生在对端。把 REASON_HANGUP 显示成“对方已挂断”是最典型的弄反。 */
typedef enum {
    WFAV_END_UNKNOWN                 = 0,
    WFAV_END_BUSY                    = 1,   /* 我方正在通话中 */
    WFAV_END_SIGNAL_ERROR            = 2,
    WFAV_END_HANGUP                  = 3,   /* 我方挂断 */
    WFAV_END_MEDIA_ERROR             = 4,
    WFAV_END_REMOTE_HANGUP           = 5,
    WFAV_END_OPEN_CAMERA_FAILURE     = 6,
    WFAV_END_TIMEOUT                 = 7,   /* 我方未在超时前接听 */
    WFAV_END_ACCEPT_BY_OTHER_CLIENT  = 8,   /* 本账号的另一个端接听了 */
    WFAV_END_ALL_LEFT                = 9,
    WFAV_END_REMOTE_BUSY             = 10,
    WFAV_END_REMOTE_TIMEOUT          = 11,  /* 对方未在超时前接听 */
    WFAV_END_REMOTE_NETWORK_ERROR    = 12,
    WFAV_END_ROOM_DESTROYED          = 13,
    WFAV_END_ROOM_NOT_EXIST          = 14,
    WFAV_END_ROOM_PARTICIPANTS_FULL  = 15,
    WFAV_END_INTERRUPTED             = 16,
    WFAV_END_REMOTE_INTERRUPTED      = 17,
    WFAV_END_KICKED                  = 18,
} wfav_end_reason_t;

/* 挂断消息里的原因是从发送方视角写的，本端显示之前必须翻转：对方的 Hangup 是
 * 我方的 RemoteHangup，对方的 Timeout 是我方的 RemoteTimeout。
 *
 * 弄反了不会让通话失败 —— 它只是每次都在屏幕上写错一句话，而这种只影响显示、
 * 却对每个用户都成立的问题，恰恰是最难被发现的。它是枚举上的纯函数，所以和枚举
 * 放在一起，也是测试首先覆盖的东西。 */
wfav_end_reason_t wfav_end_reason_flip(wfav_end_reason_t reason);

/* 给界面用的简短中文说明，不会为 NULL。从本机视角措辞，所以 REASON_HANGUP 是
 * “已挂断”，REMOTE_HANGUP 是“对方已挂断”。 */
const char *wfav_end_reason_str(wfav_end_reason_t reason);

/* ------------------------------------------------------------ 参与者状态 */

/* 某个人处在什么状态，这和整通电话处在什么状态不是同一个问题。
 *
 * 两者的生命周期塞不进同一个枚举：参与者不会有 IDLE，而 CONNECTED 对通话来说
 * 是“媒体在流动”，对一个已经进房间但什么都没发布的人来说却得表示“已在房间里”。
 * 用同一个类型会让一个旁听者读起来像是已接通，而那正是通话界面绝不能写出来的
 * 一句话。
 *
 * 每次进入房间后状态是单向推进的。停止发布的人退回 JOINED，之后可以再次到达
 * PUBLISHING。 */
typedef enum {
    /* 已被邀请、正在响铃，或已接听但还没进房间。此时从房间里还看不到关于他的
     * 任何信息。 */
    WFAV_PARTICIPANT_INVITED = 0,
    /* 已在房间里，但没有发布任何媒体流 —— Janus 称之为旁听者。他在列表里，
     * 而且是安静的。 */
    WFAV_PARTICIPANT_JOINED,
    /* 正在发布，且我方已经订阅了他的音频。订阅正在建立，但还没有声音。 */
    WFAV_PARTICIPANT_PUBLISHING,
    /* 他的媒体已经到达本机。只有这个状态下才真的听得见他。 */
    WFAV_PARTICIPANT_CONNECTED,
} wfav_participant_state_t;

const char *wfav_participant_state_str(wfav_participant_state_t state);

/* ---------------------------------------------------------------- 参与者 */

/* 通话中的另一个人，就本机所知的部分。
 *
 * 相当于各客户端的 participantProfile，去掉了这里用不上的视频字段。时间戳与
 * 其他 WFC 时间戳一样是服务器的毫秒值：join_time 是他被邀请的时刻，
 * accept_time 是他接听的时刻，两者之间的间隔就是响铃超时所度量的东西。
 *
 * 没有单独的“旁听者”标志：状态本身就说明了。PUBLISHING 之前的人都在房间里，
 * 而且是安静的。 */
typedef struct {
    char                     user_id[WFC_TARGET_MAX];
    wfav_participant_state_t state;
    int64_t                  join_time;
    int64_t                  accept_time;
    bool                     audio_muted;
} wfav_participant_t;

#ifdef __cplusplus
}
#endif

#endif /* WFAV_TYPES_H */
