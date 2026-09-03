# 自定义消息

**扩展野火 IM 的消息类型时，请勿修改 `../../../wfc-esp/` 下的内容**——
改了以后跟上游合并会很麻烦。要加的东西全在这个目录里。

这是 web 端 [`vue-chat/src/wfc_custom_message`](../../../../vue-chat/src/wfc_custom_message)
的 C 版本，步骤一一对应。

## 五步

| # | web 端 | 这里 |
|---|---|---|
| 1 | 定义消息类型 `customMessageContentType.js` | [custom_message_type.h](custom_message_type.h)，`1000` 以下是内部保留 |
| 2 | 实现消息 `testCustomMessageContent.js`（encode/decode/digest） | [custom_message_config.c](custom_message_config.c) 的发送函数 + digest 函数 |
| 3 | 配置注册 `customMessageConfig.js` | [custom_message_config.c](custom_message_config.c) 的 `CUSTOM_MESSAGES[]` |
| 4 | 实现 UI `TestCustomMessageContentView.vue` | [custom_message_view.c](custom_message_view.c) 的 `draw_test()` |
| 5 | 配置消息与 UI 的对应 `MessageContentContainerView.vue` | [custom_message_view.c](custom_message_view.c) 的 `custom_message_draw()` |

C 这边没有类，所以 web 端 `MessageContent` 子类的三个方法散落成了三处，
但都在这个目录里：

- `encode()` → `custom_message_send_*()`：决定正文写进哪个字段
- `decode()` → `custom_message_text()`：把正文读回来（**在 store 回调里跑，
  这是消息的 payload 还活着的唯一时刻**）
- `digest()` → 表里的 `.digest` 函数：会话列表那一行

## 最小的自定义消息：一行代码都不用写

把可读的正文放进 `searchable_content`（**所有野火客户端都按这个约定读**），
然后往表里加一行：

```c
{
    .type         = 1003,
    .name         = "工单",
    .persist_flag = WFC_PERSIST_PERSIST_COUNT,
    .notification = false,
    .digest       = NULL,          /* 默认就读 searchable_content */
},
```

会话列表、未读角标、聊天气泡、重启后还在——全都有了。
不注册也能收到，只是列表里显示成 `type 1003`。

按这个约定还有一个好处：**没实现这个类型的客户端（手机、Web）也能看懂**，
显示成一条普通文字，而不是「不支持的消息」。

## 三个字段的含义

- `persist_flag` 是**发给对端**的行为：bit0 不置位谁都不存，bit1 不置位谁都不计未读。
  发送时写 `WFC_PERSIST_FROM_TYPE`，值从表里取，改一处就够。
- `notification` = 居中显示的通知条，不是气泡。内置的群通知就是这么画的。
- `digest` 只在正文不在 `searchable_content` 时才需要，例子见
  `digest_test_notification()`。

## 覆盖内置类型

注册表里注册一个已有的 type 就是覆盖它。`RENAMED_BUILTINS[]` 用这个把
`image` / `call` 换成了「[图片]」「[通话]」——**组件里不放任何中文界面串**，
给人看的字归应用管。

## 在板子上试

聊天页输入框里发这两个词（[../ui_chat.c](../ui_chat.c) 的 `prime()`）：

```
/custom    发一条 1001 自定义消息（带 CUSTOM 1001 标签的气泡）
/tip       发一条 1002 自定义通知（居中一行）
```

对端用 web 或手机客户端看，会显示成「不支持的消息」或者
`searchable_content` 里那句话——这正是自定义消息在**没实现它的客户端**上的样子。

## 坑

- **注册要在 `wfc_client_connect()` 之前**。会话行的摘要是消息入库那一刻算好存下来的，
  后注册不会回头去修历史行。[app_main.c](../app_main.c) 里的调用点就在
  `wfc_client_init()` 后面。
- **`custom_message_text()` 在 store 的查询回调里跑**，`msg` 出了这个回调就悬空。
  要解 JSON 就在这儿解。它同时也拿着 store 的锁：别阻塞，别回头调 `wfc_client.h`。
- **`custom_message_draw()` 在 UI 任务上、握着显示锁跑**：只能建控件，不能发请求。
- **二进制 payload 有上限**。`MessageContent.data` 存进本地库时按
  `CONFIG_WFC_STORE_MAX_DATA`（默认 1024 字节）截断，文本字段按
  `CONFIG_WFC_STORE_MAX_TEXT`（默认 512）。截断会打 WARN 日志并写明是哪个字段——
  在线收到的那一次是完整的、重启后读回来是半截，这种 bug 不该是安静的。
- **类型号 ≥ 1000**，否则可能和野火以后新增的内置类型撞车。
