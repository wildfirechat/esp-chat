/* 强类型的事件订阅。
 *
 * 通用事件总线的自然形态是 (event_id, void *args)，每个订阅者都要以一次强制
 * 类型转换开头；等到事件携带的内容变了，编译期没有任何人报错，出问题的是桌上
 * 那块正在读一个已经不存在的结构的板子。
 *
 * 所以这里的做法是：每个事件一个具名回调类型、一个具名订阅函数。内部按这些
 * 类型的联合体保存回调，而不是转换一个通用指针，因此全程没有不受检查的一步。
 * 改动某个事件携带的内容，会直接得到一份需要一起改的调用方清单。
 *
 * ------------------------------------------------------------------------
 * 线程模型。每个回调都在触发该事件的任务上执行。服务器引起的事件 —— 消息、
 * 资料、好友列表、连接状态 —— 都在长连接任务上，也就是读取套接字的那个任务；
 * 由 API 调用引起的事件（只有 wfc_clear_unread()）则在调用方的任务上。
 * 因此，对常见情形而言：
 *
 *   - 不要在回调里阻塞。你待在回调里的这段时间，长连接是不被读取的，而服务器
 *     的心跳不会因此宽容。
 *   - 不要直接操作 LVGL。控件树属于 UI 任务，请投递过去，或用 lv_async_call。
 *   - 不要在回调里调用 wfc_client_disconnect()。它会等待你正站在上面的那个
 *     任务退出。
 *
 * 订阅和退订则可以在任意任务里调用，也可以在回调内部调用 —— 包括退订当前正在
 * 执行的这个订阅。
 *
 * 生命周期遵循 wfc_model.h：传给回调的 wfc_message_t 随该次调用失效，而资料和
 * 会话行是副本，回调可以留着用。
 */

#ifndef WFC_EVENT_H
#define WFC_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一次订阅。订阅方有可能先于程序结束而消失时，要把它保存下来 —— 典型情况是
 * 会被替换掉的 LVGL 页面，因为回调里握着一个已释放的 lv_obj_t * 会在完全无关
 * 的地方崩溃。活到程序结束的订阅方可以直接丢弃这个句柄。 */
typedef struct wfc_subscription wfc_subscription_t;

/* -------------------------------------------------------------------- 事件 */

/* 连接状态变化，取值是 wfc_client.h 里的 wfc_connection_status_t。负值表示
 * 失败且客户端不会自行重试；1 表示正常工作。 */
typedef void (*wfc_on_connection_status_t)(int status, void *ud);

/* 一批消息，从旧到新，已经入库并去过重。has_more 为 true 表示同步还有若干轮
 * 要跑，这时可以先不做开销大的重画，等它稳定下来。 */
typedef void (*wfc_on_receive_messages_t)(const wfc_message_t *msgs, size_t n,
                                          bool has_more, void *ud);

/* 自己发出的一条消息有结果了。error_code 为 0 表示发送成功，此时 uid 和
 * timestamp 是服务器分配的。 */
typedef void (*wfc_on_send_result_t)(int error_code, int64_t message_uid,
                                     int64_t timestamp, void *ud);

typedef void (*wfc_on_recall_message_t)(const char *operator_uid,
                                        int64_t message_uid, void *ud);

/* 某个会话行发生了变化：新的最后一条消息、新的未读数，或者刚被标记为已读。
 * 参数是一份副本。 */
typedef void (*wfc_on_conversation_update_t)(const wfc_conversation_info_t *info,
                                             void *ud);

/* 某个会话连同其中的消息一起被删除了。只有群会话会走到这里，原因也只有两个：
 * 主动退群，以及服务器表示这个群已经不存在。
 *
 * 它没有并入上面的更新事件、当作“更新为空”来处理，是因为这里没有内容可交付
 * —— 一个全是 0 的 wfc_conversation_info_t 仍然是一行，而这里要表达的正是这
 * 一行不再存在。订阅方拿到的是原来那个会话。 */
typedef void (*wfc_on_conversation_removed_t)(const wfc_conversation_t *conv,
                                              void *ud);

/* 刚从服务器拉到、并且已经入库的资料。数组是调用方的临时缓冲，随该次调用失效；
 * 之后要用就去存储里取。 */
typedef void (*wfc_on_user_infos_update_t)(const wfc_user_info_t *users, size_t n,
                                           void *ud);
typedef void (*wfc_on_group_infos_update_t)(const wfc_group_info_t *groups, size_t n,
                                            void *ud);
typedef void (*wfc_on_channel_infos_update_t)(const wfc_channel_info_t *channels,
                                              size_t n, void *ud);

/* group_id 这个群里有 n 个成员发生了变化。成员本身不随事件传出 —— 大群会带来
 * 一次大拷贝，而订阅方通常只想要一个群昵称 —— 需要什么请从存储里读。 */
typedef void (*wfc_on_group_members_update_t)(const char *group_id, size_t n,
                                              void *ud);

/* 好友列表变了，有 n 条记录被更新。理由同群成员：请查存储。 */
typedef void (*wfc_on_friend_list_update_t)(size_t n, void *ud);

/* 好友请求列表变了，有 n 条记录被更新。理由同上：请查存储。 */
typedef void (*wfc_on_friend_request_update_t)(size_t n, void *ud);

/* 账号的用户设置变了，有 n 条被更新。服务器成批下发设置时会触发，本客户端自己
 * 的修改被确认时也会触发，所以显示某项设置的界面不需要区分这两种情况。
 *
 * 涉及置顶、免打扰，以及本账号在某个会话里读到哪里的设置，还会额外触发该会话的
 * 会话更新事件，所以已经在监听那个事件的列表不需要再处理这个 —— 包括在手机上
 * 消掉未读的情形。 */
typedef void (*wfc_on_user_settings_update_t)(size_t n, void *ud);

/* 刚到达并已入库的回执：和自己聊天的那些人读到哪里了。数组是调用方的临时缓冲，
 * 与资料类事件一样随该次调用失效；之后要用就去存储里取，通常用
 * wfc_client.h 的 wfc_message_receipt()。
 *
 * 分成两个事件而不是一个，是因为两个列表的形状不同（见 wfc_model.h）：送达是
 * 关于人的，已读是关于某人在某个会话里的。只显示一个会话的界面需要后者，通常
 * 可以忽略前者。
 *
 * 两者都不会触发会话更新事件：回执改变的是一条消息的样子，而不是会话行的内容。 */
typedef void (*wfc_on_delivery_update_t)(const wfc_delivery_t *entries, size_t n,
                                         void *ud);
typedef void (*wfc_on_read_update_t)(const wfc_read_entry_t *entries, size_t n,
                                     void *ud);

/* 服务器推送的音视频房间事件：有人发布或取消发布了媒体流，有人加入或离开，
 * 房间被销毁等。
 *
 * event 是 Janus JSON，以 NUL 结尾，随该次调用失效 —— 它指向推送报文里解出来
 * 的内容。需要什么请在回调内部解析。
 *
 * 这是唯一一个背后没有本地数据的事件。其他事件都是“存储变了，去读吧”；房间
 * 事件则是音视频 SDK 的状态机直接消费的一条协议消息，因为进行中的通话没有对应
 * 的本地存储。 */
typedef void (*wfc_on_conference_event_t)(const char *event, void *ud);

/* -------------------------------------------------------------------- 订阅 */

/* 只有在内存分配失败时才返回 NULL。忽略返回值是可以的；要保存它的调用方则不应
 * 假定它一定非空。 */

wfc_subscription_t *wfc_on_connection_status(wfc_on_connection_status_t cb, void *ud);
wfc_subscription_t *wfc_on_receive_messages(wfc_on_receive_messages_t cb, void *ud);
wfc_subscription_t *wfc_on_send_result(wfc_on_send_result_t cb, void *ud);
wfc_subscription_t *wfc_on_recall_message(wfc_on_recall_message_t cb, void *ud);
wfc_subscription_t *wfc_on_conversation_update(wfc_on_conversation_update_t cb,
                                               void *ud);
wfc_subscription_t *wfc_on_conversation_removed(wfc_on_conversation_removed_t cb,
                                                void *ud);
wfc_subscription_t *wfc_on_user_infos_update(wfc_on_user_infos_update_t cb, void *ud);
wfc_subscription_t *wfc_on_group_infos_update(wfc_on_group_infos_update_t cb, void *ud);
wfc_subscription_t *wfc_on_channel_infos_update(wfc_on_channel_infos_update_t cb,
                                                void *ud);
wfc_subscription_t *wfc_on_group_members_update(wfc_on_group_members_update_t cb,
                                                void *ud);
wfc_subscription_t *wfc_on_friend_list_update(wfc_on_friend_list_update_t cb, void *ud);
wfc_subscription_t *wfc_on_friend_request_update(wfc_on_friend_request_update_t cb,
                                                 void *ud);
wfc_subscription_t *wfc_on_user_settings_update(wfc_on_user_settings_update_t cb,
                                                void *ud);
wfc_subscription_t *wfc_on_delivery_update(wfc_on_delivery_update_t cb, void *ud);
wfc_subscription_t *wfc_on_read_update(wfc_on_read_update_t cb, void *ud);
wfc_subscription_t *wfc_on_conference_event(wfc_on_conference_event_t cb, void *ud);

/* 传 NULL 是安全的，在被退订的那个回调内部调用也是安全的。同一个句柄只能退订
 * 一次：退订之后句柄即失效，不能再次传入。 */
void wfc_unsubscribe(wfc_subscription_t *sub);

/* 逐个退订并把数组元素置空，这样一个页面保存的一组订阅可以一行拆掉。 */
void wfc_unsubscribe_all(wfc_subscription_t **subs, size_t n);

/* 退订全部。用于程序退出流程，不适合日常使用 —— 它会把别处代码的订阅也一并
 * 退掉，而那些代码对此一无所知。 */
void wfc_event_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* WFC_EVENT_H */
