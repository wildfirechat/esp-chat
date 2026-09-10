/* 本地存储：客户端在两个报文之间记住的东西 —— 取决于编译配置，也可能是在两次
 * 开机之间记住的东西。
 *
 * 它的全部要点只有一个：上层业务不知道自己在和哪种后端打交道。两种后端各编译
 * 一种，由 Kconfig 选择：
 *
 *   CONFIG_WFC_STORE_SQLITE   建在 FATFS "storage" 分区上的数据库。同步位置和
 *                             消息都能扛过掉电，所以重连时只需向服务器要增量。
 *   CONFIG_WFC_STORE_RAM      同一套接口，实现为 PSRAM 里一个有界的环形缓冲。
 *                             不写 flash，重启后什么都不剩。
 *
 * 选 RAM 会把 SQLite、它的 VFS 以及 FATFS 挂载整个排除在固件之外：大约 700 KB
 * flash 和整个 storage 分区。这正是拆成两种后端的意义 —— 只需要显示实时消息的
 * 设备不该背着一个数据库引擎。
 *
 * 两者对外只有一处可见差异，而且是有意的：wfc_store_is_persistent()。调用方用
 * 它来说明情况（“已恢复同步位置” / “从头开始”），而不是用它来切换行为。
 *
 * ------------------------------------------------------------------------
 * 线程模型：这里每个函数都会拿存储自己的互斥锁，所以任意任务都可以调用任意
 * 函数。该锁是可重入的，因此查询回调里可以再读存储；但回调里不能写存储，那会
 * 让正在进行的遍历失效。回调也不该阻塞 —— 它正挡着所有想用存储的其他任务。
 *
 * 生命周期：wfc_store_put_message() 会把要保存的内容全部复制一份，所以调用方
 * 的 wfc_message_t 可以照常指向解码后的报文（见 wfc_model.h）。反过来，传给
 * 查询回调的 wfc_message_t 只在该次调用内有效。
 *
 * 其余内容 —— 会话、用户、群组、群成员、好友、好友请求、用户设置 —— 都是拷进
 * 拷出的，调用方拿到的是自己的副本，可以留着用。
 */

#ifndef WFC_STORE_H
#define WFC_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wfc_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 生命周期 */

typedef struct {
    /* 这份存储属于哪个账号。用与写入时不同的用户 ID 打开时，存储会被清空而不是
     * 合并：里面的同步位置索引的是另一个账号的时间线，从那里继续拉取会悄悄漏掉
     * 消息。 */
    const char *user_id;

    /* 加密数据库文件用的密钥，仅在开启了 CONFIG_WFC_STORE_ENCRYPT 时使用：它是
     * 应用服务器下发 token 的第三段 dbSecret，由 wfc_client_init() 填入 ——
     * token 在本地解密，所以联网之前就能给存储配好密钥。
     *
     * 未开启加密的固件会忽略它。开启了加密却没有给它，会导致打开失败，而不是
     * 悄悄写明文。 */
    const char *db_secret;
} wfc_store_config_t;

/* 挂载（SQLite）或分配内存（RAM），让存储可用。用同一个用户重复调用是空操作。
 *
 * SQLite 挂载或打开失败是致命错误：退回 RAM 会看上去一切正常，然后在每次重启时
 * 丢掉同步位置。怎么处理由调用方决定。 */
esp_err_t wfc_store_open(const wfc_store_config_t *cfg);
void      wfc_store_close(void);

bool wfc_store_is_open(void);

/* 本次编译是否会写 flash。实际上是编译期常量，做成函数是为了调用方不必写
 * #ifdef。 */
bool wfc_store_is_persistent(void);

/* "sqlite" 或 "ram" —— 给状态面板和开机日志用。 */
const char *wfc_store_name(void);

/* 丢弃所有消息和所有键值。换账号时会用到，也可以作为应用的“恢复出厂设置”。 */
esp_err_t wfc_store_clear(void);

/* -------------------------------------------------------------------- 键值 */

/* 各类同步位置。每个都是服务器分配的单调递增版本号，把它跨重启保存下来，才能让
 * 重连变成一次增量同步而不是重新下载一遍。
 *
 * 这几个位置是同一套机制写出来的，而每次请求所依据的版本就是这个键本身，不会
 * 从数据行里反推：用户设置是客户端也会写的列表，表里最新的 update_dt 有可能是
 * 自己刚写的，据此推出来的位置会跑到服务器前面，从而漏掉另一台设备在此期间的
 * 修改。两个回执列表则从另一面说明了同一件事 —— 它们的应答自带版本号，数据行
 * 里根本没有可以推导版本的东西。
 *
 * WFC_KEY_GROUP_CONV_HEAD 是特殊的一个：它是超级群会话列表的版本，而不是某条
 * 消息线的位置；排在它后面的是每个会话一个的位置，见下面的
 * wfc_store_set_group_head()。 */
#define WFC_KEY_MSG_HEAD        "msg_head"
#define WFC_KEY_FRIEND_HEAD     "friend_head"
#define WFC_KEY_FRIEND_RQ_HEAD  "friend_rq_head"
#define WFC_KEY_SETTING_HEAD    "setting_head"
#define WFC_KEY_RECV_HEAD       "recv_head"
#define WFC_KEY_READ_HEAD       "read_head"
#define WFC_KEY_GROUP_CONV_HEAD "group_conv_head"

#define WFC_STORE_KEY_MAX 24

esp_err_t wfc_store_set_i64(const char *key, int64_t value);

/* 键从未写入过时返回 fallback —— 包括 RAM 存储的每一次开机，“还没有同步位置”
 * 就是这样传给调用方的，不需要另外一条“未找到”的分支。 */
int64_t wfc_store_get_i64(const char *key, int64_t fallback);

/* -------------------------------------------------------------------- 消息 */

/* 这个消息 ID 是否已经存过。这就是去重：同一条消息可能到达两次，一次是推送，
 * 一次是随后同步的那一批，而两者带着同一个服务器分配的 ID。
 *
 * 只有真正存下来的消息才会返回 true —— 关于 persist_flag 见
 * wfc_store_put_message()。 */
bool wfc_store_has_message(int64_t message_uid);

/* 存一条消息，把调用方缓冲区里的字符串全部复制一份。
 *
 * 存下来返回 ESP_OK；有意不存时返回 ESP_ERR_INVALID_STATE：content.persist_flag
 * 的 bit 0 为 0 的消息是透传消息（比如“正在输入”），其他客户端也一样会丢弃它。
 * 两者都不是调用方需要处理的失败。
 *
 * 已经存在的消息 ID 会保持原样并返回 ESP_OK。
 *
 * 超过 CONFIG_WFC_STORE_MAX_MESSAGES 时淘汰最旧的消息。这个上限是针对整个存储
 * 的，不是每个会话。 */
esp_err_t wfc_store_put_message(const wfc_message_t *msg);

/* 把接下来的一串写操作合成一个事务。
 *
 * SQLite 后端里，事务之外的每一次写都自带一个事务，而在这块 flash 上一个事务
 * 就是一次日志写加两次 fsync。一条消息要付两次（消息行，加上被它顶到最上面的
 * 会话行）—— 对刚到的一条消息无所谓，对首次同步一轮拉回来的几百条就是灾难。
 *
 * begin 成功返回 true。**事务横跨这两个调用，锁不横跨**——每次写照旧自己拿一次
 * 锁再放掉，所以读的人能从两条消息中间挤进去，看到的是这条连接尚未提交的行，
 * 也就是列表一边到一边出来。真正让读者等的只有 end 里的那次提交。所以：
 *   - 这两个调用之间只能做存储的事，不能去拿别的锁（客户端的 s_lock 尤其不行，
 *     那是 ASSESSMENT.md 8.6 明令禁止的顺序），也不能阻塞；
 *   - 一批不要太长。一批跑多久，最后那次提交就把存储锁占多久，UI 任务的
 *     wfc_store_hold() 就有多久拿不到、这一轮不画。几十条一批既省下绝大部分
 *     fsync，又短到看不出来。
 *   - 一轮同步该写的 head 也放进来（wfc_impl.c 的 deliver()）：它自己写就是
 *     又一个事务，而且会先于它所代表的那些消息落盘。
 *
 * begin 返回 false 表示没开成（存储还没打开），此时**不要**调用 end。
 * 事务开不起来不是失败：写照样会发生，只是回到一次一个事务。
 * RAM 后端里两个都是空函数。 */
bool wfc_store_batch_begin(void);
void wfc_store_batch_end(void);

/* 先把存储占住，再读一批，读完释放 —— 给"不能在这里等"的读者用。
 *
 * 这里每个函数本来就自己拿锁，所以正常情况下调用方什么都不用管。有一种情况例外：
 * UI 任务是**拿着显示锁**在读存储的，而显示锁也是 LVGL 的锁。补拉的时候写一页要
 * 几十毫秒，一次提交要几秒，这段时间里任何一次读都会等 —— 于是面板既不重画也不
 * 读触摸，看起来就是死机而不是变慢。
 *
 * 所以那种调用方要反过来：先用一个短超时占住存储，占不到就这一轮不画，占到了再去
 * 拿显示锁。锁是可重入的，所以中间那些读直接命中已经持有的锁，一次都不会等。
 *
 * 顺序只有这一个方向：存储 -> 显示。反过来（拿着显示锁去等存储）正是要避免的那件
 * 事。返回 false 表示超时，此时**不要**调用 release。 */
bool wfc_store_hold(uint32_t timeout_ms);
void wfc_store_release(void);

/* 当前存了多少条消息。 */
uint32_t wfc_store_message_count(void);

/* 回调返回 false 可以提前结束遍历。 */
typedef bool (*wfc_store_message_cb_t)(const wfc_message_t *msg, void *ud);

/* 遍历已存储的消息，从新到旧，最多 limit 条。
 *
 * conv 传 NULL 表示遍历所有会话，开机时重放消息记录用的就是这种形式；传具体
 * 会话则按 (type, target, line) 过滤。
 *
 * 注意顺序：查询时从新到旧最合适，但界面通常要从旧到新地画，所以调用方要自己
 * 收集并反转。 */
esp_err_t wfc_store_query_messages(const wfc_conversation_t *conv, size_t limit,
                                   wfc_store_message_cb_t cb, void *ud);

/* 按服务器消息 ID 取一条消息。没有这条消息时返回 false，此时不会调用 cb；
 * cb 最多执行一次，因此其返回值被忽略。
 *
 * 它与 wfc_store_has_message() 是一对：后者问的是同一个唯一索引上的同一个问题，
 * 只是只回答有或没有。留着一个消息 ID 的调用方 —— 而消息 ID 也正是一条消息上
 * 最值得留下来的东西，它在重画和重启之间都不变 —— 不该为了找回那一行而去遍历
 * 整个会话。
 *
 * msg 与遍历时一样是借用内存的：所有字符串随回调结束失效（见 wfc_model.h），
 * 要留下的内容必须在这里复制走。正因如此，它才是取那些长到不适合缓存的字段的
 * 正确方式 —— 媒体地址最长可达 CONFIG_WFC_STORE_MAX_TEXT，而完整内容只在存储
 * 里。 */
bool wfc_store_get_message(int64_t message_uid, wfc_store_message_cb_t cb, void *ud);

/* -------------------------------------------------------------------- 会话 */

/* 会话列表不需要调用方维护：它是消息表的投影，由 wfc_store_put_message() 顺带
 * 更新。真正插入成功的消息会把所属会话移到最前、在会话行上留下摘要，如果是收到
 * 的消息且 persist_flag 表示计数，还会增加未读数。
 *
 * 把这件事放在存储层而不是业务层，才能让两种后端保持一致，也让不变量容易表述：
 * 列表反映的就是存储里的内容。重复的消息（推送来一次、同步又拉到一次）不会插入
 * 任何东西，因此也不会计数 —— 这正是插入操作必须报告“到底有没有插入”的原因。
 *
 * 超过 CONFIG_WFC_STORE_MAX_CONVERSATIONS 的会话按最后活跃时间从旧到新丢弃。 */

typedef bool (*wfc_store_conversation_cb_t)(const wfc_conversation_info_t *info, void *ud);

/* 遍历会话列表，从新到旧 —— 这就是它该被画出来的顺序，所以与消息查询不同，
 * 调用方不需要反转。 */
esp_err_t wfc_store_query_conversations(size_t limit, wfc_store_conversation_cb_t cb,
                                        void *ud);

/* 复制出一行会话。该会话还没有任何消息时返回 false，且不改动 out。 */
bool wfc_store_get_conversation(const wfc_conversation_t *conv,
                                wfc_conversation_info_t *out);

uint32_t wfc_store_conversation_count(void);

/* 所有会话的未读数与 @ 数之和，也就是总角标。 */
uint32_t wfc_store_total_unread(void);

/* 把一个会话标记为已读：两个计数都清零。纯本地操作 —— 上报服务器（让对方和本
 * 账号的其他端保持一致）是 wfc_client.h 里的 wfc_clear_unread()，它会先调用这
 * 个函数再上报。
 *
 * 反方向（在另一台设备上读过）不需要任何调用：它会作为一条
 * WFC_SETTING_CONVERSATION_SYNC 设置到达，由上面的设置写入函数应用。 */
esp_err_t wfc_store_clear_unread(const wfc_conversation_t *conv);

/* 忘掉一个会话及其中的消息。服务器表示某个群已经不存在时会用到；界面上的
 * “删除会话”也应该调用它。 */
esp_err_t wfc_store_remove_conversation(const wfc_conversation_t *conv);

/* -------------------------------------------------------------------- 资料 */

/* 界面把 ID 变成名字所需的三种缓存：用户、群组、群内昵称。三者随时都能从服务器
 * 重新拉取，所以它们是严格意义上的缓存 —— 丢了只需要一次往返，不会丢数据。
 *
 * 每张表都是有界的（每张表 CONFIG_WFC_STORE_MAX_PROFILES，每个群
 * CONFIG_WFC_STORE_MAX_GROUP_MEMBERS），按缓存时间从旧到新淘汰。设备通常只和
 * 少数几个人来往，实际上不会淘汰到任何东西；设这个上限是为了不让一个大规模部署
 * 把 flash 填满没人会看的资料。
 *
 * 写入函数按 ID 覆盖，也是数据行出现的唯一途径。 */

esp_err_t wfc_store_put_user(const wfc_user_info_t *user);

/* 用户不在缓存里时返回 false，且不改动 out。要不要为此发一次拉取由调用方决定
 * —— wfc_client.h 的 wfc_get_user_info() 就是会拉的那个。 */
bool wfc_store_get_user(const char *user_id, wfc_user_info_t *out);

esp_err_t wfc_store_put_group(const wfc_group_info_t *group);
bool      wfc_store_get_group(const char *group_id, wfc_group_info_t *out);

/* 合并一批群成员。每个成员自带 group_id，服务器下发的是自请求版本以来发生变化
 * 的所有成员 —— 所以这是更新而不是替换，其中类型为“已移除”的成员表示删除，而
 * 不是一行要保留的记录。
 *
 * 整批一次调用，是为了让 SQLite 后端可以把它包进一个事务：两百人的群写一次日志
 * 即可，而不是两百次。 */
esp_err_t wfc_store_put_group_members(const wfc_group_member_t *members, size_t n);

bool wfc_store_get_group_member(const char *group_id, const char *member_id,
                                wfc_group_member_t *out);

typedef bool (*wfc_store_group_member_cb_t)(const wfc_group_member_t *member, void *ud);

esp_err_t wfc_store_query_group_members(const char *group_id, size_t limit,
                                        wfc_store_group_member_cb_t cb, void *ud);

/* 本地为这个群保存的最大 update_dt，也就是下次拉取群成员时要带上的版本。没有
 * 缓存时为 0，表示要整份成员列表。 */
int64_t wfc_store_group_member_max_dt(const char *group_id);

/* 上面三种缓存之外的第四种，性质相同：频道的名字，随时可以从服务器重新拉取。
 * 它与用户表分开，是因为频道 ID 不是用户 ID —— 拿它去查用户什么也查不到，这正
 * 是没有这张表时频道会话只能显示 <channelId> 的原因。 */
esp_err_t wfc_store_put_channel(const wfc_channel_info_t *channel);
bool      wfc_store_get_channel(const char *channel_id, wfc_channel_info_t *out);

/* ------------------------------------------------------ 超级群的同步位置 */

/* 超级群消息线上、每个会话各自的同步位置（见 wfc_model.h）。
 *
 * 分成两个写入函数而不是一个，是因为两个值来自不同的地方，而且谁都不能覆盖对方：
 * server_head 来自服务器对“有什么”的应答，head 来自“我们拉到了什么”。合成一个
 * “写入整行”的调用，会要求每个调用方同时持有两个值，而只有其中一个值的调用方会
 * 把另一个清零 —— 那读起来就是一个永远没有新消息的会话。 */
esp_err_t wfc_store_set_group_head(const char *group_id, int32_t line, int64_t head);
esp_err_t wfc_store_set_group_server_head(const char *group_id, int32_t line,
                                          int64_t server_head);

/* 本地已经拉到哪里。这个群还没有对应的行时为 0，因此第一次拉取会从头开始。 */
int64_t wfc_store_group_head(const char *group_id, int32_t line);

typedef bool (*wfc_store_group_head_cb_t)(const wfc_group_head_t *entry, void *ud);

/* 只遍历那些落后的会话 —— 即 server_head > head 的 —— 因为这正是下次拉取要问
 * 的名单，已经追平的行没必要出现在里面。回调返回 false 可以提前结束。 */
esp_err_t wfc_store_query_group_heads(size_t limit, wfc_store_group_head_cb_t cb,
                                      void *ud);

/* -------------------------------------------------------------------- 好友 */

/* 服务器下发的是自所给版本以来变化的全部内容，包括已经结束的关系。与下面的群
 * 成员不同，下次要用的版本不是从这些行推出来的，而是 WFC_KEY_FRIEND_HEAD。 */
esp_err_t wfc_store_put_friends(const wfc_friend_t *friends, size_t n);

bool wfc_store_get_friend(const char *user_id, wfc_friend_t *out);

typedef bool (*wfc_store_friend_cb_t)(const wfc_friend_t *entry, void *ud);

/* 只遍历当前的好友：关系已结束的记录留着是为了下次拉取不再重复下发，而不是为了
 * 列出来。 */
esp_err_t wfc_store_query_friends(size_t limit, wfc_store_friend_cb_t cb, void *ud);

/* ---------------------------------------------------------------- 好友请求 */

/* 与好友列表完全一样：自所给版本以来变化的全部内容，包括已处理的请求。一条请求
 * 以 (from_uid, to_uid) 为键 —— 同样两个人在两个方向上各可以有一条未处理的
 * 请求。 */
esp_err_t wfc_store_put_friend_requests(const wfc_friend_request_t *requests, size_t n);

/* 按这一对用户取一条请求。没有时返回 false，且不改动 out。
 *
 * 这个查询是为写入路径准备的，不是为显示准备的：处理一条好友请求时，客户端会在
 * 收到应答后在本地写下新的状态，而写回之前必须先读出原来那一行，否则请求方填的
 * 附言 —— 这条记录里唯一给人看的部分 —— 会被空串覆盖，直到下次拉取才恢复。 */
bool wfc_store_get_friend_request(const char *from_uid, const char *to_uid,
                                  wfc_friend_request_t *out);

typedef bool (*wfc_store_friend_request_cb_t)(const wfc_friend_request_t *entry,
                                              void *ud);

/* 两个方向都遍历，包括已处理的 —— 一条已同意的请求仍然是列表想显示的内容。
 * 顺序是缓存顺序，与好友遍历一样：两种后端都不排序，需要按时间倒序的调用方自己
 * 排。 */
esp_err_t wfc_store_query_friend_requests(size_t limit,
                                          wfc_store_friend_request_cb_t cb, void *ud);

/* ---------------------------------------------------------------- 用户设置 */

/* 第三个由同步位置驱动的列表，也是唯一一个有写入路径的：整份拉取会填满这张表，
 * 单项修改会往里加一行，两者都走这里。
 *
 * 数据行以 (scope, key) 为键，所有 scope 都保留，包括本客户端根本不认识的 ——
 * 它们属于账号而不属于这台设备，客户端把它们丢掉，会导致一次往返之后本设备与
 * 旁边的手机显示得不一样。
 *
 * 三个会话相关的 scope 不只是存起来：写入置顶或免打扰会像写入消息一样更新它
 * 指向的会话行；而写入已读位置 —— 本账号最近一次打开该会话的那台设备写的 ——
 * 会重算该行的未读数，在手机上读过的会话，角标就是这样在这里消失的。这份投影
 * 放在存储层而不是业务层，是为了一处实现、两种后端通用，并且让会话列表不可能与
 * 产生它的设置相矛盾。 */
esp_err_t wfc_store_put_user_settings(const wfc_user_setting_t *settings, size_t n);

/* 账号没有这一项设置时返回 false，且不改动 out。 */
bool wfc_store_get_user_setting(int32_t scope, const char *key,
                                wfc_user_setting_t *out);

typedef bool (*wfc_store_user_setting_cb_t)(const wfc_user_setting_t *entry, void *ud);

/* scope 中的全部设置；scope 为 WFC_SETTING_SCOPE_ANY 时是本地保存的全部设置。
 * 顺序是缓存顺序，同好友遍历。 */
esp_err_t wfc_store_query_user_settings(int32_t scope, size_t limit,
                                        wfc_store_user_setting_cb_t cb, void *ud);

/* -------------------------------------------------------------------- 回执 */

/* 第四、第五个由同步位置驱动的列表：送达回执和已读回执（wfc_model.h 说明了两者
 * 各自的含义以及形状为何不同）。
 *
 * 两个写入函数都取“已存的值和新值中较大的那个”，而不是直接覆盖。服务器的时间点
 * 只会前进，但乱序到达的增量 —— 重连与推送撞在一起 —— 会让回执倒退，把一条已经
 * 读过的消息重新变成未读。
 *
 * 两张表都不是任何东西的投影：回执是在要显示它的地方、通过与消息时间戳比较来读
 * 的，所以不像置顶和免打扰那样需要同步维护某个会话行。两张表都以
 * CONFIG_WFC_STORE_MAX_PROFILES 为界，量级也相同 —— 有来往的每个人一行，已读则
 * 是每人每会话一行。 */

esp_err_t wfc_store_put_deliveries(const wfc_delivery_t *entries, size_t n);

/* 这个用户没有记录时返回 0，读作“我们的消息还没有到达他”，两种理解下都是调用方
 * 需要的结果。 */
int64_t wfc_store_delivery_dt(const char *user_id);

esp_err_t wfc_store_put_reads(const wfc_read_entry_t *entries, size_t n);

int64_t wfc_store_read_dt(const wfc_conversation_t *conv, const char *user_id);

typedef bool (*wfc_store_read_cb_t)(const wfc_read_entry_t *entry, void *ud);

/* 本地保存的、conv 中所有人的已读位置。单聊最多一条；群里则是每个读过内容的
 * 成员一条，“3 人已读”数的就是它。顺序是缓存顺序，同好友遍历。 */
esp_err_t wfc_store_query_reads(const wfc_conversation_t *conv, size_t limit,
                                wfc_store_read_cb_t cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* WFC_STORE_H */
