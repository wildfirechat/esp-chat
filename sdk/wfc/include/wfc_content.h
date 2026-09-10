/* 消息类型表：一个消息类型意味着什么，由这张表说了算。
 *
 * 它相当于其他 WFC 客户端的 registerMessageContent()，存在的理由也一样。一个
 * 部署会自造消息类型 —— 工单、传感器读数、门磁通知 —— 而客户端必须在不重新
 * 编译 SDK 的前提下知道关于它们的三件事：发送时 persist_flag 该写什么、它是
 * 提示还是气泡、以及它在会话列表里的一行摘要长什么样。
 *
 * 因此 SDK 内置的类型和应用注册的类型放在同一张表里，形式完全相同，在要紧的
 * 地方也没有区别：存储写会话行摘要时、聊天页面判断“这是不是一条提示”时、
 * 发送时带上的存储标志。
 *
 * ------------------------------------------------------------------------
 * 注册。在启动时调用一次 wfc_register_content_type()（或它的复数形式），位置
 * 在 wfc_client_init() 之后、wfc_client_connect() 之前 —— 也就是还没有任何
 * 消息往来的时候，并且只在一个任务里调用。这张表会被 wfc 任务读（消息到达时
 * 要算摘要），也会被 UI 任务读（页面要画那个摘要）；在它们运行期间注册就是
 * 数据竞争。这里没有加锁，因为启动时注册不需要锁。
 *
 * 注册一个已经存在的类型是替换，内置类型也可以替换 —— 应用因此可以把自己的
 * 措辞放到界面上（显示“[图片]”而不是 "image"），而不必改 SDK。SDK 不附带任何
 * 中文文案也是这个原因：内置类型的名字是给日志看的 ASCII 标签，给人看的文字
 * 由应用决定。
 *
 * name、digest 以及结构里其他指针都不会被复制。请注册静态存储 —— 文件作用域的
 * const 数组是预期的写法。
 *
 * 按 WFC 的约定，1000 以下的类型属于协议，1000 及以上属于部署方。这里不做强制
 * 检查；遵守它可以避免自定义类型与 WFC 以后新增的类型冲突。
 */

#ifndef WFC_CONTENT_H
#define WFC_CONTENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 部署方自定义类型的起始编号。 */
#define WFC_CONTENT_CUSTOM_FIRST 1000

/* 用在 wfc_content_out_t.persist_flag 上，表示“用这个类型注册时声明的标志”；
 * 类型没有注册过时退回为“持久化并计入未读”。
 *
 * 它是一个哨兵值而不是 0，因为 0 本身是一个合法标志（WFC_PERSIST_NONE，投递
 * 之后哪里都不保存），而一个“零值另有含义”的发送 API 是个陷阱。所以用注册过的
 * 类型组装消息时应该写成：
 *
 *     wfc_content_out_t out = { .type = MY_TYPE,
 *                               .persist_flag = WFC_PERSIST_FROM_TYPE,
 *                               .searchable_content = "..." }; */
#define WFC_PERSIST_FROM_TYPE (-1)

typedef struct wfc_content_type wfc_content_type_t;

/* 这条消息的一行摘要，用于会话列表或日志。
 *
 * 它在正在存储或正在绘制的那个任务上执行 —— 消息入库时是 wfc 任务，页面重画时
 * 是 UI 任务 —— 所以它不能阻塞，也不能回调进客户端。content 随该次调用失效
 * （见 wfc_model.h），所以这里是写入调用方给的缓冲区，而不是返回指针。
 *
 * 请务必写入内容：缓冲区为空就是会话列表里空白的一行。 */
typedef void (*wfc_content_digest_fn)(const wfc_content_type_t *desc,
                                      const wfc_message_content_t *content,
                                      char *buf, size_t buf_size);

struct wfc_content_type {
    int32_t     type;
    /* 这个类型叫什么。没有更合适的内容可显示时会显示它，日志里也会打印它，
     * 所以值得起个好读的名字。 */
    const char *name;

    /* 调用方使用 WFC_PERSIST_FROM_TYPE 时，wfc_send_message() 实际写到协议里的
     * 标志。接收端会照办：不含 bit 0 的标志哪里都不存，不含 bit 1 的不计入任何
     * 人的未读数 —— 所以自定义消息在对端会不会留下痕迹，由这个字段决定。 */
    int32_t     persist_flag;

    /* 画成居中的一行提示而不是气泡：小贴士、群通知、“某某加入了群聊”。聊天页面
     * 通过它来判断，而不是去比较类型号，所以自定义的通知类消息不需要额外代码就
     * 和内置的长得一样。 */
    bool        notification;

    /* NULL 表示使用 wfc_content_digest_body：有正文就用正文，没有就用类型名。
     * 对遵循 WFC 惯例的自定义类型（把给人看的那一行放进 searchable_content）来
     * 说这就是正确的默认行为，简单的自定义消息因此完全不需要写代码。 */
    wfc_content_digest_fn digest;
};

/* -------------------------------------------------------------------- 注册 */

/* 注册或替换一个类型。表满时返回 ESP_ERR_NO_MEM（这是编译期决定的上限，不是
 * 运行时状态，日志里会指出要调大哪个配置项）。 */
esp_err_t wfc_register_content_type(const wfc_content_type_t *desc);

/* 数组形式，应用可以用一行注册整张类型表。遇到第一个失败即停止并返回。 */
esp_err_t wfc_register_content_types(const wfc_content_type_t *descs, size_t n);

/* 查这个类型的表项，没人认识这个编号时返回 NULL。先查注册的类型再查内置类型，
 * 所以注册可以覆盖内置。 */
const wfc_content_type_t *wfc_content_type_find(int32_t type);

/* -------------------------------------------------------------------- 读取 */

/* 消息类型的短名字，用于日志。不会为 NULL；没有注册过的编号返回 "unknown"。 */
const char *wfc_content_type_str(int32_t type);

/* 该类型是否画成居中提示。未注册的类型返回 false —— 未知类型按普通消息显示，
 * 这是两种猜测里比较安全的一种。 */
bool wfc_content_is_notification(int32_t type);

/* 群通知的内容里是否点名了 user_id。
 *
 * 踢人（以及其他针对具体人的群通知）的目标在 MessageContent.content 那段 JSON
 * 的 ms 数组里，而本账号在不在其中，决定了一条踢人通知只是一条消息，还是这个
 * 会话的终点。
 *
 * 这里有意做成“限定在该数组范围内的子串查找”而不是解析 JSON：本客户端没有
 * JSON 解析器，ID 是带引号的，而限定范围正是为了避免同一个 ID 在对象的别处
 * 被匹配上 —— 而它确实会出现在别处，因为踢人的操作者在 o 字段里。 */
bool wfc_group_notify_targets_user(const char *content, const char *user_id);

/* 会话列表为一条消息显示什么，以及聊天页面在没有为该类型准备专门视图时画什么。
 * 总会写入内容。
 *
 * 这是 SDK 内部唯一会用到这张类型表的地方：消息入库时存储会调用它，于是会话行
 * 上带着一条存储层已经不再保存的消息的摘要。由此带来一个需要知道的后果 ——
 * 会话行显示的是消息到达时算出来的摘要，所以在消息已经入库之后再注册类型，不会
 * 回头去重算历史记录。请在启动时注册。 */
void wfc_message_digest(const wfc_message_t *msg, char *buf, size_t buf_size);

/* ------------------------------------------------------------ 现成的摘要函数 */

/* 默认实现：消息有 searchable_content 就用它，没有就用类型名。 */
void wfc_content_digest_body(const wfc_content_type_t *desc,
                             const wfc_message_content_t *content,
                             char *buf, size_t buf_size);

/* 无论消息带了什么，一律显示类型名。内置的媒体类型和信令类型用的就是它 ——
 * 一张图片的“摘要”就是“图片”两个字。 */
void wfc_content_digest_name(const wfc_content_type_t *desc,
                             const wfc_message_content_t *content,
                             char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* WFC_CONTENT_H */
