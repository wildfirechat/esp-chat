/* IM 客户端：应用需要的接口都在这一个头文件里。
 *
 * 整套 API 是读写分离的：读接口（会话、消息、资料、好友、设置……）直接查
 * 本地存储并立即返回；写接口和刷新请求通过长连接发出，结果稍后以事件的形式
 * 通知（wfc_event.h）。除 wfc_client_connect() 外，这里没有任何接口会等待
 * 网络。
 *
 * 因此界面永远不必等服务器。画一行会话时取显示名总能立即取到 —— 资料还没
 * 拉到时是尖括号包起来的 ID —— 稍后资料到达会触发事件，届时重画那一行即可。
 *
 * ------------------------------------------------------------------------
 * 客户端会自动同步的内容：
 *
 *   消息        含超级群自己的消息线（服务端未部署超级群时不会产生请求）
 *   会话列表    由已存储的消息投影而来，包含未读数
 *   资料        用户、群组、群成员、频道，按需拉取
 *   好友        好友列表与好友请求
 *   用户设置    含会话置顶、免打扰
 *   回执        已读、送达，仅在服务端开启该功能时同步
 *
 * 可以修改的服务端数据：用户设置（含本账号的已读位置）、好友关系、群成员。
 * 解散群、改群名、设置管理员与禁言、黑名单暂未提供。
 *
 * ------------------------------------------------------------------------
 * 断线重连由客户端自己完成。
 *
 * 连接断开后按 2 秒起、逐次加倍、最长 1 分钟的退避（带抖动）重试，直到重新
 * 连上或应用调用 wfc_client_disconnect()。应用只会看到状态变化：
 * UNCONNECTED、每次重试时的 CONNECTING、然后 CONNECTED，随后是一次普通的
 * 增量同步。
 *
 * 有五种拒绝不会重试，因为重发同样的请求不会得到别的结果：token 无效、
 * token 与本 client_id 不匹配、账号被封禁、服务端 license 不接受本客户端，
 * 以及被其他端顶下线（重连会把会话再抢回来）。这些状态会停留在连接状态上，
 * 界面可以据此说明原因；要再次连接需应用显式调用 wfc_client_connect()。
 *
 * WFC_STATUS_TIME_INCONSISTENT 是唯一会重试的拒绝：设备时钟不对只有一个
 * 原因 —— SNTP 还没同步完 —— 几秒后自己就好了，第一次失败就放弃反而要重启
 * 才能恢复。
 */

#ifndef WFC_CLIENT_H
#define WFC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_content.h"
#include "wfc_event.h"
#include "wfc_model.h"
#include "wfc_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 连接状态 */

/* 取值与其他 WFC 客户端（connectionStatus）逐个对应，本客户端不会产生的状态
 * 在这里留空号而不是改作他用。负值表示失败。 */
typedef enum {
    WFC_STATUS_TIME_INCONSISTENT   = -9, /* 本机时钟与服务器相差太多 */
    WFC_STATUS_NOT_LICENSED        = -8, /* 服务端 license 不接受本客户端，
                                          * 或本固件内置的 license 已过期 */
    WFC_STATUS_KICKED_OFF          = -7, /* 被其他端顶下线 */
    /* token 不是这个 client_id 的。昨天还能连、今天连不上时先查这一项：
     * token 和 client_id 由应用服务器成对下发，把 token 和别人的 client_id
     * 配在一起就是这个现象。 */
    WFC_STATUS_SECRET_KEY_MISMATCH = -6,
    WFC_STATUS_TOKEN_INCORRECT     = -5,
    WFC_STATUS_SERVER_DOWN         = -4, /* 路由或长连接连不上 */
    WFC_STATUS_REJECTED            = -3, /* 账号被封禁，或服务端拒绝登录 */
    WFC_STATUS_LOGOUT              = -2, /* 已调用 disconnect()，不再重连 */
    WFC_STATUS_UNCONNECTED         = -1,
    WFC_STATUS_CONNECTING          = 0,
    WFC_STATUS_CONNECTED           = 1,  /* 已连接，所有功能可用 */
    /* 已连接，正在追赶历史消息：消息成批到达、会话列表还在变，界面可以据此
     * 提示“同步中”，而不是连续重画几百次。 */
    WFC_STATUS_RECEIVING           = 2,
} wfc_connection_status_t;

const char *wfc_status_str(wfc_connection_status_t status);

/* ------------------------------------------------------------------ 初始化 */

typedef struct {
    uint16_t    route_port;      /* 0 表示 80 */

    const char *user_id;
    const char *client_id;       /* token 与它绑定 */
    const char *token;           /* 应用服务器下发的 base64 串 */

    /* 仅在本地存储还没有消息位置时起作用 —— 首次启动、因换账号而清空过存储，
     * 或使用 CONFIG_WFC_STORE_RAM 的每一次启动。false 表示从登录时服务器给的
     * 位置开始，只收此后的新消息；true 表示从 0 开始，把服务器保留的漫游历史
     * 拉下来。
     *
     * 需要展示会话列表的应用应该用 true：会话列表不是服务端的一份数据，而是
     * 本地消息表的投影，所以一条历史都不拉就没有任何会话，只能等别人发消息
     * 才会出现。 */
    bool pull_history;

    /* 一次追赶最多连续拉取多少轮，超过后就停下等下一次消息通知。0 表示 32。 */
    int max_pull_rounds;
} wfc_client_config_t;

/* 复制配置并打开本地存储。只调用一次，在联网之前调用 —— 这样界面一起来就
 * 带着上次运行的会话列表，而不是等连上以后才填。
 *
 * 存储打不开会直接把错误返回给调用方，这种情况不能靠继续运行来兜底：同步
 * 位置将无法持久化，而消息去重就是“问存储”，去重失效会让每条推送的消息都
 * 出现两次。 */
esp_err_t wfc_client_init(const wfc_client_config_t *cfg);

/* 取路由、连接、鉴权，然后开始追赶消息；此后一直负责把连接维持住。
 *
 * 这是本头文件里唯一阻塞的接口：它要等到登录应答回来（或失败）才返回，冷启动
 * 时通常要几秒。之后的一切都在客户端自己的任务上进行，以事件的形式送达，所以
 * 它只应该在启动流程里调用一次，绝不能在回调里调用。
 *
 * 返回值只是第一次尝试的结果，失败并不代表结束：只要不是上面说的那几种不会
 * 重试的拒绝，函数返回时重连循环已经在跑了 —— 把这个错误当致命错误处理，等于
 * 丢掉一台几秒后就能自己连上的设备。应该改为观察连接状态。
 *
 * 调用前系统时钟必须已经正确。每个加密报文都带一个“自 2018-01-01 起的小时数”
 * 前缀，服务器会校验它，所以 SNTP 未完成时连接不是变慢，而是直接失败。 */
esp_err_t wfc_client_connect(void);

/* 停止重连，发送断开报文并拆掉连接。不要在事件回调里调用：它要等待回调所在的
 * 任务退出。 */
void wfc_client_disconnect(void);

wfc_connection_status_t wfc_client_status(void);

/* 当前登录的账号；wfc_client_init() 之前返回 ""。 */
const char *wfc_client_user_id(void);

/* -------------------------------------------------------------------- 消息 */

/* 发一条文本消息：即以文本类型、并带上会计入对方未读数的标志调用
 * wfc_send_message()。
 *
 * 报文发出即返回；服务器的应答以发送结果事件送达，消息也只有在应答给出消息 ID
 * 之后才会入库 —— 在此之前它没有可用于去重的 ID。
 *
 * 任意任务均可调用，包括 UI 任务。 */
esp_err_t wfc_send_text(const wfc_conversation_t *conv, const char *text);

/* -------------------------------------------------------------- 自定义消息 */

/* 由程序拼出来而不是用户敲出来的消息：应用自定义的类型、通话邀请、接听、挂断
 * 等。字段是 wfc_message_content_t 加上两个只有发送方才有的字段。它不持有任何
 * 内存：所有指针都属于调用方，且只在发送调用期间被读取。
 *
 * 所有字符串传 NULL 等同于 ""。要点是 type 和 persist_flag：服务器按发送方给
 * 的标志处理，所以标记为 WFC_PERSIST_PERSIST_COUNT 的消息会计入对方未读数，
 * 而标记为 WFC_PERSIST_TRANSPARENT 的通话邀请两端都不存储、不留通话记录。
 * WFC_PERSIST_FROM_TYPE（wfc_content.h）表示取该类型注册时声明的标志，注册过
 * 自定义类型的应用应该用它 —— 标志写在类型表里一处，而不是在每个发送点重复。 */
typedef struct {
    int32_t        type;
    int32_t        persist_flag;
    const char    *searchable_content;
    const char    *push_content;
    /* 离线推送要携带的数据，与 push_content 不是一回事。例如通话邀请把通话 ID
     * 和参与者放在这里，好让休眠中的手机能响铃。 */
    const char    *push_data;
    const char    *content;
    const char    *extra;
    const uint8_t *data;
    size_t         data_len;

    /* 媒体字段，供内容本身不在消息里、而是指向已上传文件的自定义类型使用：
     * media_type 是 WFC 的媒体类型编号，remote_media_url 是文件地址。两者只是
     * 原样转发，本客户端不负责上传。 */
    int32_t        media_type;
    const char    *remote_media_url;

    /* @ 提醒。1 表示 @所有人，2 表示 @mentioned_targets 里的人。它不只是显示
     * 效果：接收端会把被 @ 的消息计入 wfc_conversation_info_t.unread_mention
     * 而不是普通未读数，“有人@我”的标记就是据此画出来的。 */
    int32_t        mentioned_type;
    const char *const *mentioned_targets;
    size_t         n_mentioned_targets;
} wfc_content_out_t;

/* 一次发送的结果，只回调给发起这次发送的调用方。
 *
 * 发送结果事件（wfc_event.h）会告诉所有订阅者“有消息发出去了”，适合状态面板；
 * 而状态机需要知道的是“我自己发的那一条”的结果 —— 通话邀请的消息 ID 之后要用
 * 在接听和挂断里。两者都会发生：每次发送都触发事件，传了回调的发送另外还会带着
 * 自己的 ud 回调一次。
 *
 * 与其他应答一样在长连接任务上执行。error_code 为 0 表示发送成功，此时 uid 和
 * timestamp 是服务器分配的。 */
typedef void (*wfc_send_result_cb_t)(int error_code, int64_t message_uid,
                                     int64_t timestamp, void *ud);

/* 把 content 发到 conv，也可以只发给会话中指定的几个人。这是唯一的发送通道：
 * wfc_send_text() 是它的封装，音视频 SDK 的信令走它，应用自定义的消息类型也
 * 走它。
 *
 * to_users 是定向消息：消息只发给这些用户，而不是会话里的所有人。通话信令全都
 * 用它 —— 接听只发给主叫和本账号的其他端，而不是发给一个群；传 NULL/0 表示
 * 普通的会话广播。
 *
 * 本地行为：服务器应答之后，这条消息与收到的消息一样入库 —— 会话行更新摘要、
 * 列表把它移到最前、并触发会话更新事件。存储与否由 persist_flag 决定，规则与
 * 接收端一致：透传的通话邀请哪里都不存，声明为持久化的自定义消息会出现在消息
 * 记录里并在重启后仍在。应答之前什么都不存：没有服务器给的消息 ID 就没法与
 * 下次同步收回来的同一条消息去重。
 *
 * cb 可以为 NULL，不影响发送结果事件。任意任务均可调用。
 *
 * 返回值不是 ESP_OK 时不会回调 cb，ud 仍归调用方所有。 */
esp_err_t wfc_send_message(const wfc_conversation_t *conv,
                           const wfc_content_out_t *content,
                           const char **to_users, size_t n_to_users,
                           wfc_send_result_cb_t cb, void *ud);

/* ---------------------------------------------------------- 音视频房间信令 */

/* 一次房间请求的应答。response 是服务器转回来的 Janus JSON，以 NUL 结尾，只在
 * 回调期间有效；请求失败或没有返回内容时为 NULL。
 *
 * error_code 是传输层的应答码，不是 Janus 的错误码：请求到达了 Janus 而被它
 * 拒绝时，error_code 是 0，拒绝的原因在 response 里（data.error_code）。两者
 * 都要看。 */
typedef void (*wfc_conference_reply_cb_t)(int error_code, const char *response,
                                          void *ud);

/* 音视频控制通道：在房间主题上发出一条会议请求，并把应答带回来。
 *
 * WFC 的 Janus 信令全部走这里。IM 服务器为 Janus 做代理，客户端不需要另开连接：
 * create_room、join_pub、message（携带 SDP）、trickle、join_sub、keepalive、
 * leave 都是“请求名 + 一段 JSON”，Janus 的应答在报文应答里回来。服务器主动推送
 * 的房间事件走另一条路，即 wfc_on_conference_event()。
 *
 * session_id 在 create_room 或 join_pub 返回之前为 0。data 是请求的 JSON，不带
 * 参数的请求（如 keepalive）传 ""。advance 是“高级会议”标志，1v1 通话传 false。
 *
 * 任意任务均可调用。只要返回 ESP_OK，cb 一定且只会被调用一次。 */
esp_err_t wfc_send_conference_request(int64_t session_id, const char *room_id,
                                      const char *request, const char *data,
                                      bool advance,
                                      wfc_conference_reply_cb_t cb, void *ud);

/* 已存储的消息，从新到旧，最多 limit 条。conv 传 NULL 表示遍历所有会话。
 * 直接转发给存储层，放在这里是为了界面只需要包含本头文件。 */
esp_err_t wfc_get_messages(const wfc_conversation_t *conv, size_t limit,
                           wfc_store_message_cb_t cb, void *ud);

/* 按服务器消息 ID 取一条消息。没有这条消息时返回 false；cb 最多执行一次，其
 * 返回值被忽略。
 *
 * 用途：界面保存的是“行”而不是消息 —— 消息是借用内存的（wfc_model.h），出了
 * 回调就失效，所以想用某个当初没复制下来的字段时只能再查一次。反过来把更多
 * 字段复制进行结构里正是这个接口要避免的坑：行是定长的、每次重画都会重建，
 * 真正长的字段（媒体地址可长达 CONFIG_WFC_STORE_MAX_TEXT）会在那里被悄悄截断。
 *
 * 开销不大 —— 走的是与去重相同的唯一索引 —— 但也不是免费的，而且要拿存储锁。
 * 请在负担得起的地方调用：每个真正需要的地方查一次，而不是每次重画的每一行都
 * 查一次。 */
bool wfc_get_message(int64_t message_uid, wfc_store_message_cb_t cb, void *ud);

/* 同步进度：拉到哪里了，是否还在同步中。 */
int64_t  wfc_message_head(void);
bool     wfc_is_syncing(void);
uint32_t wfc_received_count(void);
uint32_t wfc_sent_count(void);

/* -------------------------------------------------------------------- 会话 */

/* 会话列表，从新到旧 —— 也就是它该被画出来的顺序。 */
esp_err_t wfc_get_conversations(size_t limit, wfc_store_conversation_cb_t cb, void *ud);

bool wfc_get_conversation_info(const wfc_conversation_t *conv,
                               wfc_conversation_info_t *out);

/* 总未读数：所有会话的未读数与 @ 数之和。
 *
 * 未读数是账号的，不是这台设备的：在手机上读过的会话，其已读位置也会同步到这里
 * 来，并以会话更新事件的形式通知。 */
uint32_t wfc_get_unread_count(void);

/* 把一个会话标记为已读：未读数清零、触发会话更新事件重画列表，如果原本有未读，
 * 还会上报服务器。
 *
 * 上报这一步同时做两件事（见 wfc_model.h 中的 WFC_SETTING_CONVERSATION_SYNC）：
 * 它是本账号的已读位置，手机上那一端会因此不再为这个会话显示未读；它同时也是
 * 发送方收到的已读回执。后半件事只在服务端支持回执且用户没有关闭回执时发生，
 * 前半件事总会发生。
 *
 * 它不阻塞，也不在调用方的任务上发送 —— 上报交给连接管理去做，这既使它可以在
 * 界面构建过程中调用，也使离线时的已读操作不会丢失。代价是它不是可靠投递的：
 * 在连接恢复前断电会丢掉这次上报，对方会继续把消息显示为未读，直到本设备再次
 * 打开这个会话。 */
esp_err_t wfc_clear_unread(const wfc_conversation_t *conv);

/* -------------------------------------------------------------------- 资料 */

/* 从缓存里取。refresh 为 true，或缓存里没有，都会顺带向服务器发起拉取 —— 所以
 * 缓存未命中时本次返回 false，稍后以用户资料更新事件送达。整套读模型就是这一句
 * 话。
 *
 * 请求是合并的：同一批回调里的多次未命中会合成一次拉取，这在一次同步带回二十个
 * 人的四十条消息时很重要。 */
bool wfc_get_user_info(const char *user_id, bool refresh, wfc_user_info_t *out);

/* 同上，取群组资料。若群成员未缓存、或缓存的成员版本比群信息声明的旧，还会顺带
 * 拉取群成员。 */
bool wfc_get_group_info(const char *group_id, bool refresh, wfc_group_info_t *out);

esp_err_t wfc_get_group_members(const char *group_id, size_t limit,
                                wfc_store_group_member_cb_t cb, void *ud);

/* 同上，取频道资料，一次一个频道。频道会话的标题只能来自这里：它的 target 是
 * 频道 ID，用户缓存和群组缓存都认不出来。 */
bool wfc_get_channel_info(const char *channel_id, bool refresh,
                          wfc_channel_info_t *out);

/* 一个用户该显示的名字，优先级依次为：
 *
 *   群昵称    他在这个群里给自己起的名字   （传了 group_id 时）
 *   好友备注  我给他起的名字
 *   昵称      他的昵称
 *   用户名    他的登录名
 *   <user_id> 尖括号，让“还没拉到资料”一眼可辨
 *
 * 总会写入一些内容。不在群里时 group_id 传 NULL 或 ""。
 *
 * 它不会发起拉取：名字缺失是因为资料没缓存，而这个函数每次重画的每一行都要调
 * 一次，若让它去问服务器，等于每次滚动都伴随一串拉取请求。正确做法是界面出现时
 * 调用 wfc_get_user_info() 预热缓存，再由事件触发重画。 */
void wfc_get_display_name(const char *user_id, const char *group_id,
                          char *buf, size_t buf_size);

/* 一个会话该显示的标题：群会话用群名，频道会话用频道名，单聊用对方的显示名。
 * 总会写入一些内容 —— 什么都还认不出来时写尖括号包起来的 target，这就是“该去
 * 预热缓存并重画”的信号。 */
void wfc_get_conversation_title(const wfc_conversation_t *conv, char *buf,
                                size_t buf_size);

/* -------------------------------------------------------------------- 好友 */

esp_err_t wfc_get_friends(size_t limit, wfc_store_friend_cb_t cb, void *ud);

bool wfc_is_friend(const char *user_id);

/* 双向的好友请求，包括已处理过的。请求的方向要用 from_uid 与
 * wfc_client_user_id() 比较得出，协议里没有方向字段。 */
esp_err_t wfc_get_friend_requests(size_t limit, wfc_store_friend_request_cb_t cb,
                                  void *ud);

/* ------------------------------------------------------------ 关系类写操作 */

/* 下面八个写接口是上面两个列表和群组缓存的另一半：本头文件里其他接口要么读本地
 * 存储，要么请求服务器刷新它，而这几个改变的是服务器上的数据。
 *
 * 它们共用同一套约定：
 *
 *   服务器同意之前不写本地。请求发出去，只有在应答说成功之后才写入本地的那一行。
 *   被拒绝的修改就是没有发生过，而不是先显示出来再撤回。
 *
 *   本地那一行通常只是抢先一步，而不是最终结果：服务器随后会把这次变更也通知
 *   本账号（好友、好友请求或群通知消息），后到的增量会覆盖它。两个例外是
 *   wfc_send_friend_request()（服务器只通知被请求的人）和 wfc_quit_group()
 *   （通知的是群里其他人），这两种情况下本地写入就是全部。
 *
 *   客户端自己写的行版本号为 0。已存储资料上的 update_dt 是服务器的版本号，会
 *   原样带进下一次请求；在那里填一个自己编的数字会让服务器回答“没有更新”，真正
 *   的记录就永远拉不回来了。
 *
 * 应答在长连接任务上回调 cb，因此适用 wfc_event.h 的规则：不要阻塞，不要碰
 * 控件。error_code 为 0 表示成功，222 表示批量操作只成功了一部分（群相关接口），
 * 其余为服务器错误码。cb 可以为 NULL。
 *
 * 每个函数的 esp_err_t 返回值只说明请求有没有提交出去：连接断开时为
 * ESP_ERR_INVALID_STATE，参数不合法为 ESP_ERR_INVALID_ARG，超过
 * WFC_OP_MEMBERS_MAX 为 ESP_ERR_INVALID_SIZE。不是 ESP_OK 时不会回调 cb。 */
typedef void (*wfc_operation_cb_t)(int error_code, void *ud);

/* 一次群操作最多可以指定多少人。设备是从通讯录页面上的几个人建群的，上百人的
 * 群还是在有键盘的地方组织。超过这个数直接拒绝而不是截断：悄悄地把十二个人里的
 * 十一个拉进群，比一个都没拉更糟。 */
#define WFC_OP_MEMBERS_MAX 32

/* 发起好友请求。reason 是对方会看到的附言，可以为 NULL。 */
esp_err_t wfc_send_friend_request(const char *user_id, const char *reason,
                                  wfc_operation_cb_t cb, void *ud);

/* 处理别人发来的好友请求。user_id 是发起方，即这一对里不是本账号的那一方。
 * 接受之后双方成为好友，稍后会以好友列表更新事件送达。 */
esp_err_t wfc_handle_friend_request(const char *user_id, bool accept,
                                    wfc_operation_cb_t cb, void *ud);

/* 删除好友，双向生效：服务器会清掉两个账号上的关系，对方也不再能看到本账号。 */
esp_err_t wfc_delete_friend(const char *user_id, wfc_operation_cb_t cb, void *ud);

/* 设置好友备注名，它在所有显示名字的地方优先于对方的昵称
 * （见 wfc_get_display_name）。传 "" 表示清除。 */
esp_err_t wfc_set_friend_alias(const char *user_id, const char *alias,
                               wfc_operation_cb_t cb, void *ud);

/* -------------------------------------------------------------------- 群组 */

/* 创建群组并取回群 ID。
 *
 * 群 ID 由服务器分配，无论 members 里有没有本账号，服务器都会把它设为群主，
 * 并且由服务器自己发出“创建了群组”的通知消息 —— 所以这里不需要客户端再发一条。
 * 未开启自定义群通知的服务端会直接拒绝客户端发的群通知。
 *
 * cb 拿到的新群 ID 只在该次回调内有效。回调执行时群资料已经入库，
 * wfc_get_group_info() 可以立即取到；会话行则在通知消息到达时出现。 */
typedef void (*wfc_create_group_cb_t)(int error_code, const char *group_id,
                                      void *ud);

esp_err_t wfc_create_group(const char *name, const char **members, size_t n_members,
                           wfc_create_group_cb_t cb, void *ud);

/* 拉人入群 / 把人移出群。
 *
 * 谁有权做哪一项由服务器判断，取决于群类型以及本账号是不是群主或管理员 ——
 * 被拒绝时以错误码返回，客户端不做预判。 */
esp_err_t wfc_add_group_members(const char *group_id, const char **members,
                                size_t n_members, wfc_operation_cb_t cb, void *ud);
esp_err_t wfc_kick_group_members(const char *group_id, const char **members,
                                 size_t n_members, wfc_operation_cb_t cb, void *ud);

/* 退出群组。
 *
 * 本客户端同时会删掉这个会话及其中的消息：一个既发不了消息、也不会再收到消息的
 * 会话，不值得留在一份最多几十行的列表里。订阅方会收到会话删除事件
 * （wfc_event.h）。 */
esp_err_t wfc_quit_group(const char *group_id, wfc_operation_cb_t cb, void *ud);

/* ---------------------------------------------------------------------- 锁 */

/* WFC 的分布式锁：锁在服务器上，用一个两端约定好的字符串命名。
 *
 * 它放在这里而不是放在用到它的功能里，是因为其他 WFC 客户端也都把它放在客户端
 * 对象上，而且它本身与任何具体功能无关：服务端实现就是一张带过期时间的 map 上
 * 的 putIfAbsent，锁名的含义完全由取同一个名字的双方决定。
 *
 * 本仓库里唯一的使用者是对讲：它把锁当作两人频道的麦克风 —— 抢到锁的人说话，
 * 另一方被告知频道忙。
 *
 * duration_s 是服务器在自动释放前保留这把锁的时间。这个过期时间就是全部的安全
 * 网 —— 持锁时崩溃的客户端不会把频道永久锁死 —— 所以它应该是几秒而不是几分钟，
 * 需要更久的持有方再申请一次即可（同一账号重复申请是续期而不是失败）。
 *
 * 值得记住的两个错误码：
 *
 *   25  锁在别人手上（申请时）
 *   26  这把锁不是你的（释放时）
 *
 * 约定与上面八个写接口相同：返回 ESP_OK 表示请求已发出，cb 在长连接任务上带回
 * 服务器的应答，本地不保存任何状态 —— 没有本地锁表，因为一份靠猜维护的持锁人
 * 记录还不如每次都问服务器。 */

/* 够放下 "WFPTT_" 加两个用户 ID，这是本仓库里最长的锁名。超长直接拒绝：截断后
 * 的锁 ID 指的是另一把锁，而抢到错误的锁和抢到正确的锁看起来一模一样。 */
#define WFC_LOCK_ID_MAX 96

esp_err_t wfc_require_lock(const char *lock_id, int32_t duration_s,
                           wfc_operation_cb_t cb, void *ud);
esp_err_t wfc_release_lock(const char *lock_id, wfc_operation_cb_t cb, void *ud);

/* -------------------------------------------------------------------- 回执 */

/* 对方读到哪里了。与这里其他读接口一样直接查本地存储；填充存储的是送达回执和
 * 已读回执的同步，而它们只在服务端开启了回执功能时才会进行 —— 所以在没有该功能
 * 的服务端上，下面每个接口都回答“还没有”，这既是实话，也省掉了调用处的分支。
 *
 * 两个列表的形状不同，原因见 wfc_model.h：送达是每个人一个时间点，已读是每个人
 * 在每个会话里一个时间点。 */

/* 回执在这里有没有意义：服务端开启了该功能，且用户没有关闭回执。界面可以据此
 * 整列不画，而不是画一排永远不会变化的标记。 */
bool wfc_is_receipt_enabled(void);

/* 没有记录时返回 0，读作“还没有”，这正是调用方需要的结果。 */
int64_t wfc_get_delivery(const char *user_id);
int64_t wfc_get_read(const wfc_conversation_t *conv, const char *user_id);

/* 本地保存的、conv 中所有人的已读位置。 */
esp_err_t wfc_get_reads(const wfc_conversation_t *conv, size_t limit,
                        wfc_store_read_cb_t cb, void *ud);

/* 单聊中，自己发出的一条消息该显示什么状态：用上面两个接口与消息时间戳比较，
 * 并按优先级取值（已读蕴含已送达，所以已读优先）。
 *
 * 群会话一律返回 WFC_RECEIPT_SENT，这是有意的 —— “已送达”在二十个人之间没有
 * 单一含义，群里想要的是一个数字，那个数字是 wfc_message_read_count()。 */
typedef enum {
    WFC_RECEIPT_SENT      = 0,   /* 服务器已收到，还没有人收到 */
    WFC_RECEIPT_DELIVERED = 1,
    WFC_RECEIPT_READ      = 2,
} wfc_receipt_t;

wfc_receipt_t wfc_message_receipt(const wfc_conversation_t *conv, int64_t timestamp);

/* 有多少人已经读到 timestamp。单聊和群会话都适用，单聊返回 0 或 1。 */
size_t wfc_message_read_count(const wfc_conversation_t *conv, int64_t timestamp);

/* ---------------------------------------------------------------- 用户设置 */

/* 账号级的设置，属于账号而不属于这台设备：置顶了哪些会话、静音了哪些会话，以及
 * 手机或桌面端设置过的其他内容。拉取是整份拉，修改是一次改一项，账号登录的每一
 * 端都会看到这次修改。
 *
 * 读接口与这里其他读接口一样只查本地存储，不会发起请求。从未设置过的项就是不
 * 存在 —— 返回 false，buf 为空 —— 默认值因此不需要第二条代码路径。
 *
 * 写接口不等待：发出去就返回，本地那一行在服务器确认后才出现，届时会触发用户
 * 设置更新事件。所以被拒绝的修改什么都不会发生，这比先显示再撤回诚实。返回
 * ESP_OK 的含义是“已发送”，不是“已生效”。
 *
 * 写接口不能在 LVGL 回调里调用：它会走到长连接上的阻塞发送。 */
bool wfc_get_user_setting(int32_t scope, const char *key, char *buf, size_t buf_size);

esp_err_t wfc_get_user_settings(int32_t scope, size_t limit,
                                wfc_store_user_setting_cb_t cb, void *ud);

esp_err_t wfc_set_user_setting(int32_t scope, const char *key, const char *value);

/* 置顶与免打扰，两个与会话有关的设置。它们都通过 wfc_set_user_setting() 写入，
 * 键由 (type, line, target) 拼成；服务器确认后会话行会更新，随之而来的会话更新
 * 事件会让列表重新排序。线程限制同上。 */
esp_err_t wfc_set_conversation_top(const wfc_conversation_t *conv, bool top);
esp_err_t wfc_set_conversation_silent(const wfc_conversation_t *conv, bool silent);

#ifdef __cplusplus
}
#endif

#endif /* WFC_CLIENT_H */
