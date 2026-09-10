/* 数据模型：WFC 的各类数据从协议里解出来之后长什么样。
 *
 * 这些是 wfcmessage.proto 中 Message、MessageContent、Conversation、User、
 * GroupInfo、GroupMember、Friend 等类型的 C 结构，只保留本客户端实际会读的
 * 字段。没有列出的字段仍在协议里，以后需要时再加，不影响收发代码。
 *
 * 这里有两套生命周期约定，区别是有意的：
 *
 *   wfc_message_t 是借用内存的。它的 const char * 字段指向解码后的报文缓冲区，
 *   所以传给回调的消息只在该次回调期间有效，出了回调就不能再用。需要留下的
 *   内容请自行复制。
 *
 *   其余结构都是自有内存的。资料和会话行都很小、定长，且读远多于写，所以它们
 *   是存储层拷进拷出的普通结构体，调用方可以放在栈上随便用。
 *
 * 这个区分是按访问频率来的：一条消息解码一次、看一次、存起来；而一个显示名，
 * 每次重画的每一行都要取一次。
 */

#ifndef WFC_MODEL_H
#define WFC_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- 会话 */

/* 会话类型。一个会话由 (type, target, line) 唯一确定：单聊的 target 是用户 ID，
 * 群聊的是群 ID；line 是 WFC 在同一 target 上的第二个维度，除非服务端用到它，
 * 否则一律为 0。 */
typedef enum {
    WFC_CONV_SINGLE   = 0,
    WFC_CONV_GROUP    = 1,
    WFC_CONV_CHATROOM = 2,
    WFC_CONV_CHANNEL  = 3,
    WFC_CONV_SECRET   = 5,
} wfc_conv_type_t;

#define WFC_TARGET_MAX 64

typedef struct {
    int  type;                      /* wfc_conv_type_t */
    char target[WFC_TARGET_MAX];
    int  line;
} wfc_conversation_t;

/* -------------------------------------------------------------- 消息类型 */

/* 消息内容类型，取值与其他 WFC 客户端一致。本客户端只渲染文本和各类通知，
 * 其余类型原样收下，界面上按类型号显示。 */
#define WFC_CONTENT_UNKNOWN            0
#define WFC_CONTENT_TEXT               1
#define WFC_CONTENT_VOICE              2
#define WFC_CONTENT_IMAGE              3
#define WFC_CONTENT_LOCATION           4
#define WFC_CONTENT_FILE               5
#define WFC_CONTENT_VIDEO              6
#define WFC_CONTENT_STICKER            7
#define WFC_CONTENT_LINK               8
#define WFC_CONTENT_P_TEXT             9
#define WFC_CONTENT_USER_CARD          10
#define WFC_CONTENT_RECALL_NOTIFY      80
#define WFC_CONTENT_TIP_NOTIFY         90
#define WFC_CONTENT_TYPING             91
/* 104..124 是各种群通知：创建群、加人、改群名等等。 */
#define WFC_CONTENT_GROUP_NOTIFY_FIRST 104
#define WFC_CONTENT_GROUP_NOTIFY_LAST  124

/* 这二十一种群通知里有三种不只是拿来显示的：它们是群在告诉本账号"你已经不在
 * 群里了"。所有 WFC 客户端都直接根据这三种通知处理，而不是事后再查一次群成员
 * —— 区别在于会话是当场消失，还是要等别的动作才发现。
 *
 * 一条通知是不是针对“我”的，每种类型判断方式不同，这也是最容易出错的地方：
 * 解散针对所有人，主动退群针对发送者，而踢人的目标在消息内容的 ms 数组里。 */
#define WFC_CONTENT_GROUP_KICK_NOTIFY    106
#define WFC_CONTENT_GROUP_QUIT_NOTIFY    107
#define WFC_CONTENT_GROUP_DISMISS_NOTIFY 108

/* 400..417 是通话控制消息，它们就是普通的 IM 消息 —— 邀请、接听、挂断，走的
 * 收发通道和一行文字完全相同。这正是音视频 SDK 不需要自己的传输层的原因：
 * wfav 订阅 wfc_on_receive_messages()，按类型把它们挑出来。
 *
 * 它们的负载是 MessageContent.data 里的 base64(JSON)，由音视频 SDK 解析。
 * 之所以列在这里而不是列在那边，是因为存储和会话摘要也需要知道 400 是一通
 * 电话。 */
#define WFC_CONTENT_VOIP_START           400  /* 通话邀请   */
#define WFC_CONTENT_VOIP_ACCEPT          401  /* 接听       */
#define WFC_CONTENT_VOIP_END             402  /* 挂断       */
#define WFC_CONTENT_VOIP_SIGNAL          403  /* 通话信令   */
#define WFC_CONTENT_VOIP_MODIFY          404  /* 修改通话   */
#define WFC_CONTENT_VOIP_ACCEPT_T        405  /* 接听（T）  */
#define WFC_CONTENT_VOIP_ADD_PARTICIPANT 406
#define WFC_CONTENT_VOIP_MUTE_VIDEO      407
#define WFC_CONTENT_VOIP_MULTI_ONGOING   416
#define WFC_CONTENT_VOIP_JOIN_REQUEST    417

/* 消息的存储标志，取值与其他 WFC 客户端一致。由发送方决定、服务器照办，所以
 * 一条文本消息必须写 3，否则不会计入任何人的未读数。 */
#define WFC_PERSIST_NONE            0
#define WFC_PERSIST_PERSIST         1
#define WFC_PERSIST_PERSIST_COUNT   3
#define WFC_PERSIST_TRANSPARENT     4

typedef struct {
    int32_t     type;
    int32_t     persist_flag;
    int32_t     media_type;
    int32_t     mentioned_type;
    /* 文本消息里是正文；媒体类消息里是会话列表要显示的摘要。不会为 NULL ——
     * 字段缺失时是 ""。 */
    const char *searchable_content;
    const char *push_content;
    const char *content;    /* 与类型相关，通常是 JSON */
    const char *remote_media_url;
    const char *extra;
    const uint8_t *data;    /* 与类型相关的二进制负载 */
    size_t         data_len;
} wfc_message_content_t;

/* -------------------------------------------------------------------- 消息 */

typedef enum {
    WFC_DIRECTION_SEND    = 0,
    WFC_DIRECTION_RECEIVE = 1,
} wfc_direction_t;

typedef struct {
    wfc_conversation_t    conversation;
    char                  from[WFC_TARGET_MAX];
    /* 服务器给这条消息分配的 ID，以及服务器的毫秒时钟。两者都由服务器决定：
     * 自己发的消息在应答里带回来，同步下来的消息里本来就有。 */
    int64_t               message_uid;
    int64_t               timestamp;
    int                   direction;   /* wfc_direction_t */
    wfc_message_content_t content;
} wfc_message_t;

/* -------------------------------------------------------------------- 资料 */

/* 够放下服务器实际下发的昵称和头像地址。放不下的值按 UTF-8 边界截断而不是丢弃：
 * 截短的名字仍能认出是谁，而另一种结果是一行没有名字的会话。 */
#define WFC_NAME_MAX 64
#define WFC_URL_MAX  128

/* 用户资料。只保留客户端要显示的字段：WFC 还带有手机号、邮箱、地址、公司、
 * 社交账号和一个自由文本字段，本客户端都不显示，而每份缓存的资料都要为它们
 * 付出存储空间。它们仍在协议里，界面需要时再加字段。
 *
 * update_dt 是服务器给这份资料的版本号，不是墙上时钟。它会原样带进下一次拉取
 * 请求，意思是“有比我手上更新的吗”，通常得到的回答是没有。 */
typedef struct {
    char    uid[WFC_TARGET_MAX];
    char    name[WFC_NAME_MAX];          /* 登录名，服务端内唯一 */
    char    display_name[WFC_NAME_MAX];  /* 昵称，客户端显示的就是它 */
    char    portrait[WFC_URL_MAX];
    int32_t type;                        /* 0 普通用户，1 机器人，2 设备 */
    int32_t gender;
    int64_t update_dt;
} wfc_user_info_t;

/* 群类型。 */
typedef enum {
    WFC_GROUP_NORMAL     = 0,
    WFC_GROUP_FREE       = 1,
    WFC_GROUP_RESTRICTED = 2,
} wfc_group_type_t;

typedef struct {
    char    target[WFC_TARGET_MAX];   /* 群 ID */
    char    name[WFC_NAME_MAX];
    char    portrait[WFC_URL_MAX];
    char    owner[WFC_TARGET_MAX];
    int32_t type;                     /* wfc_group_type_t */
    int32_t member_count;
    int64_t update_dt;
    /* 群成员自己的版本号。有人加入、退出或改了群昵称时它会变，而群信息本身
     * 可以没有变化。拿它和本地成员表的版本比较，就能判断要不要再拉一次群成员。 */
    int64_t member_update_dt;
} wfc_group_info_t;

/* 群成员类型。“已移除”不是删除：服务器保留这一行，好让每个客户端都知道这个人
 * 离开了，所以画成员列表时要把它过滤掉。 */
typedef enum {
    WFC_MEMBER_NORMAL  = 0,
    WFC_MEMBER_MANAGER = 1,
    WFC_MEMBER_OWNER   = 2,
    WFC_MEMBER_MUTED   = 3,
    WFC_MEMBER_REMOVED = 4,
    WFC_MEMBER_ALLOWED = 5,
} wfc_group_member_type_t;

typedef struct {
    char    group_id[WFC_TARGET_MAX];
    char    member_id[WFC_TARGET_MAX];
    char    alias[WFC_NAME_MAX];   /* 群昵称，优先于其他所有名字 */
    int32_t type;                  /* wfc_group_member_type_t */
    int64_t update_dt;
} wfc_group_member_t;

/* 频道资料。
 *
 * 频道是会话可以指向的第三种对象（见 wfc_conv_type_t）。它的 target 是频道 ID
 * 而不是用户 ID，所以用户缓存里永远查不到，没有这份资料时频道会话只能显示
 * <channelId>。
 *
 * 和用户资料一样做了裁剪：描述、密钥、回调地址和菜单列表都留在协议里，这里只有
 * 画一行会话要用的字段，加上标识和版本两项。
 *
 * status：0 公开，1 私有，2 已关闭。 */
typedef struct {
    char    target[WFC_TARGET_MAX];   /* 频道 ID */
    char    name[WFC_NAME_MAX];
    char    portrait[WFC_URL_MAX];
    char    owner[WFC_TARGET_MAX];
    int32_t status;
    int64_t update_dt;
} wfc_channel_info_t;

/* 超级群自己那条消息线拉到哪里了。
 *
 * 普通群没有这个东西：它们的消息走账号唯一的那条消息线，用 WFC_KEY_MSG_HEAD
 * 就够了。超级群则是每个 (群, line) 一个位置，单独拉取 —— 所以每个超级群会话
 * 对应一行，这一对值决定了还有没有东西可拉：
 *
 *   head         本地已经拉到的最新消息 ID
 *   server_head  服务器声称它持有的最新消息 ID
 *
 * server_head > head 就是“落后了”，而落后与否正是查询要问的全部。 */
typedef struct {
    char    group_id[WFC_TARGET_MAX];
    int32_t line;
    int64_t head;
    int64_t server_head;
} wfc_group_head_t;

/* 好友关系状态：0 是好友，其他值表示关系已结束。服务器会把已结束的关系也发下来
 * —— 删除就是这样传播的 —— 所以这个状态必须读，不能假定。1 是任一方删除对方时
 * 服务器写入的值，在这里列出来是因为删除好友的应答返回后，客户端会先在本地写下
 * 同样的行，而不是等好友变更推送把它带回来。 */
#define WFC_FRIEND_STATE_FRIEND  0
#define WFC_FRIEND_STATE_DELETED 1

typedef struct {
    char    uid[WFC_TARGET_MAX];
    char    alias[WFC_NAME_MAX];   /* 我给他起的备注名，优先于对方的昵称 */
    int32_t state;
    int32_t blacked;
    int64_t update_dt;
} wfc_friend_t;

/* 好友请求状态：0 才是要处理的那个，即还没人回应的请求。另外两个是处理结果，
 * 保留而不是丢弃，是因为它们的 update_dt 也是下次拉取要用的版本的一部分。 */
#define WFC_FRIEND_RQ_PENDING  0
#define WFC_FRIEND_RQ_ACCEPTED 1
#define WFC_FRIEND_RQ_REJECTED 2

/* 一条好友请求，两个方向都可能。服务器两个方向都会下发 —— 自己发出的请求就是
 * 这样收到“对方同意了”的 —— 而协议里没有方向字段，所以区分方向只能拿 from_uid
 * 和本账号的用户 ID 比较。
 *
 * 协议里还带有双方的已读状态和一个附加字段，这里没有取，等到需要画未读标记时
 * 再加，规则见本文件开头。 */
typedef struct {
    char    from_uid[WFC_TARGET_MAX];
    char    to_uid[WFC_TARGET_MAX];
    char    reason[WFC_NAME_MAX];   /* 请求方填的附言 */
    int32_t status;
    int64_t update_dt;
} wfc_friend_request_t;

/* ---------------------------------------------------------------- 用户设置 */

/* 一条用户设置是 (scope, key) -> value，它也是服务器上唯一一份客户端会写入的
 * 数据：拉取是整份拉，修改是一次加一项，账号登录的每一端都会看到这次修改。
 * “把这个会话置顶”因此是账号的属性，而不是某台设备的属性。
 *
 * key 的含义取决于 scope。两个会话相关的 scope 里，它是 "type-line-target"
 * （见下面的 wfc_conversation_setting_key）；全局开关则为空串。协议里 value
 * 一律是字符串，即使内容是数字，所以这里也是字符串 —— "1"、"0"、优先级等等。
 *
 * 这里只列出本客户端会主动处理的 scope。账号设置过的其他内容照样会同步下来、
 * 存起来，也能从 wfc_get_user_setting() 取出来：知道 scope 6 对自己意味着什么
 * 的应用，不需要 SDK 认识它就能读写。 */
#define WFC_SETTING_CONVERSATION_SILENT 1
#define WFC_SETTING_GLOBAL_SILENT       2
#define WFC_SETTING_CONVERSATION_TOP    3
/* 本账号在一个会话里读到哪里了，也是唯一一个界面不该直接设置的 scope：写它等于
 * 声明“我已经读到这里”，它既是本账号的多端同步状态，也（因为请求里带上了被读
 * 消息的发送者）是对方收到的已读回执。只有 wfc_clear_unread() 应该写它。
 * 它的值是十进制的毫秒时间戳，不是开关。
 *
 * 这个 scope 是双向读的。收到的一行是本账号的另一端写的 —— 比如手机打开了本
 * 设备正显示着未读的会话 —— 存储层会据此把未读数消掉（见 wfc_store.h）。
 * “未读数属于账号而不属于设备”指的就是这件事。 */
#define WFC_SETTING_CONVERSATION_SYNC   7
/* 用户关闭了回执的客户端会把它置为 "1"。上报已读之前要先读它：账号已经表示
 * 不希望告诉对方。 */
#define WFC_SETTING_DISABLE_RECEIPT     13
/* 服务端自定义的 scope 从这里开始，以免与 WFC 以后新增的 scope 冲突。 */
#define WFC_SETTING_CUSTOM_BEGIN        1000

/* 传给 wfc_get_user_settings() 表示遍历所有 scope。 */
#define WFC_SETTING_SCOPE_ANY (-1)

/* 够放下 "1-0-<uid>"，这是各 scope 里最长的 key；value 则按“一个数字或一个短
 * 标志”来定。两者都和其他存储字符串一样按字符边界截断（见 wfc_copy_text）。 */
#define WFC_SETTING_KEY_MAX   80
#define WFC_SETTING_VALUE_MAX 64

typedef struct {
    int32_t scope;
    char    key[WFC_SETTING_KEY_MAX];
    char    value[WFC_SETTING_VALUE_MAX];
    int64_t update_dt;
} wfc_user_setting_t;

/* 会话类 scope 用来标识一个会话的 key："<type>-<line>-<target>"，这是所有
 * WFC 客户端和服务端共同约定的顺序。顺序写错不会报错 —— 它会写进一条没人读的
 * 设置，同时悄悄丢掉原来那条。函数总会写入结尾的 NUL。
 *
 * 反向函数对不是会话 key 的输入返回 false，这种情况确实存在：本客户端不认识的
 * scope 可以用任意格式作为自己的 key。 */
void wfc_conversation_setting_key(const wfc_conversation_t *conv, char *buf,
                                  size_t buf_size);
bool wfc_conversation_from_setting_key(const char *key, wfc_conversation_t *out);

/* -------------------------------------------------------------------- 回执 */

/* 对方读到哪里了。WFC 用两个列表来表示，而不是在每条消息上放一个标志 —— 而且
 * 两个列表的形状不同，这是最需要弄清楚的一点：
 *
 *   送达是按人记的，与会话无关。服务器给每个用户维护一个时间点："发给他的、
 *   这个毫秒之前的消息都已经到达他的某个客户端了"。所以自己在 T 时刻发的消息，
 *   只要那个人的时间点越过了 T 就算已送达，与它在哪个会话里无关。
 *
 *   已读是按 (会话, 人) 记的。读是一个人对一个会话做的动作，所以群里每个成员
 *   一条，单聊正好一条。
 *
 * 两者都是单调递增的，也都只拿来和消息的时间戳比较；它们都不针对某一条具体的
 * 消息，所以 wfc_message_t 上没有对应的字段。两个列表都只在服务端开启了回执
 * 功能时才同步。 */
typedef struct {
    char    uid[WFC_TARGET_MAX];
    int64_t dt;
} wfc_delivery_t;

typedef struct {
    wfc_conversation_t conversation;
    char               uid[WFC_TARGET_MAX];
    int64_t            dt;
} wfc_read_entry_t;

/* ---------------------------------------------------------------- 会话列表 */

/* 会话列表中的一行：会话本身、最后一次有消息的时间、有多少未读，以及最后一条
 * 消息的摘要 —— 有了摘要，画一行就不必再回消息表里查。
 *
 * 摘要是存下来的而不是现查的，有两个原因：它比消息活得久 —— 一个安静的会话在
 * 最后那条消息被清理之后仍能显示最后一句话；它也让两种存储后端保持一致，因为
 * 内存后端根本没有“关联查询”这回事。
 *
 * unread_mention 与 unread 分开计数，并且不包含在 unread 里，这与其他 WFC
 * 客户端一致：角标显示两者之和，“有人@我”的标记只看被 @ 的那部分。
 *
 * top 和 silent 是“会话列表是消息表的投影”这句话的例外：它们来自账号的用户
 * 设置，与这个会话里发生过什么无关。把它们复制到会话行上是因为它们是在这里被
 * 读取的 —— 列表按 top 排序 —— 而存储层会在设置到达时把两者保持同步。 */
#define WFC_DIGEST_MAX 96

typedef struct {
    wfc_conversation_t conversation;
    int64_t            timestamp;        /* 最后一条消息的时间 */
    uint32_t           unread;
    uint32_t           unread_mention;
    int64_t            last_message_uid;
    int32_t            last_content_type;
    int32_t            last_direction;   /* wfc_direction_t */
    char               last_from[WFC_TARGET_MAX];
    char               digest[WFC_DIGEST_MAX];
    /* 置顶：0 表示不置顶，数值越大排得越靠前。WFC 用数字而不是开关，是为了让
     * 客户端可以做多个置顶级别；本客户端只会写 1，同时尊重其他客户端写的值。 */
    int32_t            top;
    /* 免打扰。未读数照样增加 —— 免打扰关掉的是提醒，不是计数 —— 所以变的是
     * 角标的颜色，不是它的数值。 */
    bool               silent;
} wfc_conversation_info_t;

static inline uint32_t wfc_conversation_unread_total(const wfc_conversation_info_t *info)
{
    return info->unread + info->unread_mention;
}

/* ---------------------------------------------------------------- 类型名字 */

/* 一个消息类型“是什么意思” —— 名字、存储标志、是否画成居中提示、一行摘要长
 * 什么样 —— 不在这里，而在 wfc_content.h 的类型表里，应用可以往表里加自己的
 * 类型：wfc_message_digest()、wfc_content_type_str() 和
 * wfc_content_is_notification() 都在那边。 */

/* 群成员类型的名字，用于日志和成员列表。不会为 NULL。 */
const char *wfc_group_member_type_str(int32_t type);

/* ------------------------------------------------------------------ 截断 */

/* 服务器发来的内容长度不受限，而客户端存下来的都是定长的，所以文本一定会被
 * 截断 —— 存储里、会话摘要里、界面的一行里都会。在多字节字符中间截断不是外观
 * 问题：LVGL 会把剩下的半个字符画成方块，而且这个字符串对下游而言不再是合法的
 * UTF-8。中文文本有三分之二的截断点落在字符中间，所以这是常态而不是边角情况。
 *
 * wfc_utf8_trim 返回在不超过 max 字节的前提下、不切断字符最多能保留 src 的
 * 多少字节；wfc_copy_text 完成复制并补上结尾的 NUL。 */
size_t wfc_utf8_trim(const char *src, size_t max);
void   wfc_copy_text(char *dst, size_t dst_size, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* WFC_MODEL_H */
