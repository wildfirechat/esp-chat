/* 上传文件，让消息可以指向它。
 *
 * 本客户端其他功能都是长连接上的一次往返，上传则是三步，而且只有第一步走长连
 * 接：先向服务器要一个上传地址，再把数据 PUT 或 POST 到它指定的 HTTP 服务，
 * 最后把得到的 URL 作为 remote_media_url 传给 wfc_send_message()。文件本身
 * 不走长连接。
 *
 * ------------------------------------------------------------------------
 * 要地址有两种方式，用哪一种由服务端决定。
 *
 * 先请求“预签名地址”。服务器直接返回一个已经签好名的 URL，客户端不需要再算
 * 任何东西，所以它也是对接各种对象存储时唯一可行的路径。它只有一种拒绝方式，
 * 含义精确地是“本部署自己保存媒体文件”，那正是第二种方式存在的场景 —— 所以
 * 这种拒绝是改道，不是失败。
 *
 * 第二种方式是取一个“上传凭证”，由客户端自己拼请求。这里只实现了它的自建存储
 * 分支，因为只有这一支不需要客户端为某种对象存储实现专门的签名算法，也只有这
 * 一支是第一种方式覆盖不到的。
 *
 * ------------------------------------------------------------------------
 * 这个调用会阻塞。它是一次长连接往返加一次 HTTP 往返，链路慢时可能要好几秒。
 *
 * 所以它遵守与其他阻塞调用相同的规则：不要在 wfc 的回调里调用（回调运行在传输
 * 任务上，而这个调用等待的应答正要由那个任务投递，在那里调用会一直死锁到超时），
 * 也不要在界面的构建或绘制流程里调用。请在调用方自己的任务里调用。
 *
 * ------------------------------------------------------------------------
 * 文件 key 不能随便起。
 *
 * 服务器会校验：路径的最后一段必须以本账号的用户 ID 开头，或者以该用户 ID 按
 * WFC 特有的方式 base64 之后的结果开头 —— 即标准 base64 再把 "+" "/" "=" 依次
 * 换成 "-2B" "-2F" "-3D"。不符合的 key 会被拒绝，而且不会说明原因。key 由本
 * 组件内部按同样的规则生成。
 */

#ifndef WFC_MEDIA_H
#define WFC_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 媒体类型，取值与其他 WFC 客户端一致。服务器按它分桶存放，所以它既决定文件
 * 落在哪个桶里，也影响文件名。 */
#define WFC_MEDIA_GENERAL  0
#define WFC_MEDIA_IMAGE    1
#define WFC_MEDIA_VOICE    2
#define WFC_MEDIA_VIDEO    3
#define WFC_MEDIA_FILE     4
#define WFC_MEDIA_PORTRAIT 5

/* 返回的 URL 缓冲区大小。对象存储签发的地址会在查询串里带上签名、有效期和
 * 凭证，所以这里是按那种地址来定的，而不是按“一个域名加一个文件名”。
 *
 * 它特意比 CONFIG_WFC_STORE_MAX_TEXT 大：URL 在写入存储时被截断，等于一条永远
 * 播不出来的消息，而截断在存储里是悄无声息的。在这里拿到完整的 URL，至少让
 * 调用方有机会发现。 */
#define WFC_MEDIA_URL_MAX 512

/* 上传 data，并把文件地址写入 url。
 *
 * ext 是带点的扩展名（".amr"），它不是可有可无的 —— 其他客户端都靠它来决定用
 * 什么播放器。mime 是要声明的 Content-Type，传 NULL 表示
 * "application/octet-stream"。
 *
 * 成功时返回 ESP_OK 并填好 url，否则：
 *   ESP_ERR_INVALID_STATE  未连接，或客户端尚未启动
 *   ESP_ERR_TIMEOUT        请求上传地址没有得到应答
 *   ESP_ERR_NOT_SUPPORTED  两种方式都不适用：服务器表示自己保存媒体文件，而
 *                          上传凭证描述的又不是自建存储。这属于服务端配置有
 *                          问题，日志里会给出存储类型
 *   ESP_FAIL               服务器拒绝，或 HTTP 上传失败
 */
esp_err_t wfc_media_upload(int32_t media_type, const char *ext, const char *mime,
                           const uint8_t *data, size_t len,
                           char *url, size_t url_size);

#ifdef __cplusplus
}
#endif

#endif /* WFC_MEDIA_H */
