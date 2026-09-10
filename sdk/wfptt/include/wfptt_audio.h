/* 由应用提供给本模块使用的音频通道。
 *
 * wfptt 自己不打开编解码设备。这是它与 ../wfav-esp 唯一的结构性差异（后者自带
 * 板级音频代码），而且这不是偷懒 —— 在这块板子上，只有这一种安排是正确的。
 *
 * 板上只有一条 I2S 总线、总线上挂着两个编解码器，而有三个功能要用它们：通话、
 * 语音消息和对讲。谁第二个来，谁就得被拒绝。要做到这一点，三者必须都经过同一个
 * 仲裁，而任何一个模块都不可能替另外两个持有这个仲裁 —— 所以仲裁属于应用，模块
 * 只负责申请。
 *
 * 于是应用在 wfptt_start() 时交过来的就是这些：一个产出 AMR-NB 的麦克风和一个
 * 消费 AMR-NB 的扬声器，且已经与其他要用它们的功能仲裁过。wfptt 负责协议、频道
 * 和时序。
 *
 * 格式在两个方向上都没有商量余地：AMR-NB，8 kHz，单声道，每个头字节对应一个
 * 20 ms 帧。所有 WildFire 客户端的对讲录制和播放用的都是它，产出别的格式的设备
 * 等于在跟谁都说不上话。
 *
 * ------------------------------------------------------------------------
 * 每个函数在哪个任务上被调用 —— 其中两个可以阻塞，其余的不行。
 *
 *   record_*    wfptt 任务。record_start() 和 record_stop() 可以花一点时间
 *               （stop 要等麦克风放下），但这里的任何函数都不允许等待网络。
 *   play_open   wfptt 的播放任务，它可以阻塞：它要打开编解码设备，然后按实时
 *   play_write  速度写入 —— 这正是它单独占一个任务的意义。
 *   play_close
 *
 * 两个任务都不持有显示锁，也都不是长连接的任务，所以实现可以随意操作硬件。但它
 * 不能回调进 wfptt。
 */

#ifndef WFPTT_AUDIO_H
#define WFPTT_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 打开麦克风并开始编码。立即返回；打开失败也可能表现为一次始终产不出字节的
     * 录音，本模块对两者一视同仁。
     *
     * ESP_ERR_INVALID_STATE 是预期之中的拒绝：音频通道被通话或语音消息占着。
     * 它会以 WFPTT_ERR_RECORDER_ERROR 的形式传给调用方。 */
    esp_err_t (*record_start)(void *ud);

    /* 到目前为止录到的 AMR 帧，不含文件头，以及它们有多少字节。指针在
     * record_stop() 或 record_cancel() 之前一直有效，长度只增不减 —— 本模块记着
     * 自己发到哪里了，每次只发新增的部分。
     *
     * 不含文件头，是因为一条音频消息里只有帧：数据流中间冒出来的 "#!AMR\n" 是
     * 六个字节，对端会试着把它当成一帧来解码。同一段录音在 record_stop() 里整体
     * 交还时是带头的，因为那里它是一个文件。 */
    const uint8_t *(*record_data)(void *ud, size_t *len);

    /* 停止录音，并把整段录音作为一个可播放的 .amr（含文件头）交出来，供发言之后
     * 可选发送的那条语音消息使用。返回 ESP_OK 时 amr 归本模块所有，它会把它交给
     * on_recording 回调（或者直接释放）。
     *
     * 返回其他值表示没有可用的录音，这不是错误：最常见的原因是按得太短，不足以
     * 构成一条消息（ESP_ERR_INVALID_SIZE）。发言仍然正常结束 —— 音频在发言过程
     * 中已经一块一块发出去了。 */
    esp_err_t (*record_stop)(void *ud, uint8_t **amr, size_t *len, int *seconds);

    /* 停止录音并丢弃。用于发言被放弃而不是正常结束的情况 —— 比如麦克风还开着时
     * 调用了 wfptt_stop()。 */
    void (*record_cancel)(void *ud);

    /* 为一串音频帧打开扬声器。在收到某个人第一块音频时调用，而不是在模块启动时
     * 调用，所以没人对着说话的设备永远不会碰编解码设备。 */
    esp_err_t (*play_open)(void *ud);

    /* 解码并播放一块音频，阻塞到这段音频放完为止。这个阻塞就是节拍：排在它前面
     * 的队列就是抖动缓冲，允许被填满。 */
    esp_err_t (*play_write)(void *ud, const uint8_t *amr, size_t len);

    /* 关闭扬声器，把音频通道还回去。在说话人停下时调用，本设备自己要拿频道说话
     * 时也会调用。 */
    void (*play_close)(void *ud);

    /* 传给上面每个函数。 */
    void *ud;
} wfptt_audio_t;

#ifdef __cplusplus
}
#endif

#endif /* WFPTT_AUDIO_H */
