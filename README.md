# esp-chat —— 板子上的野火聊天应用

ESP32-S3-BOX-3B 上的聊天客户端：屏幕、中文输入法、会话与消息，以及单人语音通话。
目标板 ESP32-S3-BOX-3B，ESP-IDF v6.1。

这里只有**应用**——`main/` 和一个字体组件。协议、存储、事件是 IM 客户端 `wfc`，
音视频是 `wfav`，这两个模块以**编译好的二进制组件**放在 [sdk/](sdk/)：

```
esp-chat/            这个工程（IDF project：main/ + 分区表 + sdkconfig）
├── 必需  sdk/wfc    libwfc.a + 公开头文件（协议 / 存储 / 模型 / 事件）
└── 可选  sdk/wfav   libwfav.a + 公开头文件 —— 去掉就没有语音通话
```

`sdk/` 里没有源码：一个模块一个 `.a` 加一份 `include/`，就是标准的 IDF 组件，
根 [CMakeLists.txt](CMakeLists.txt) 直接把它们挂进组件路径。归档里有什么、
编译期定死了哪些配置、对应哪个版本，见 [sdk/README.md](sdk/README.md)。

**应用这一层是完整源码**：屏幕、会话列表、聊天页、自定义消息、通话页都在 `main/`，
二次开发改的就是这里，不用碰 `sdk/`。

## 当前进度：P6 完成

P5 的验收标准是「板子上能真的聊天」，P6 加上了单人语音通话。
会话列表、聊天页、输入页和拼音输入法都在板子上跑过：

```
I (1757) ui_convs: conversations: 0, unread 0            ← 存储还没开
I (2301) ui_convs: conversations: 2, unread 1 (top: x64,10086@野火等)   ← 还没联网
I (11693) wfc: status: connecting
I (12184) wfc: resuming at stored head 573415378500190337
```

开机屏幕就是满的（第 2301 ms 那行在联网之前），点会话进聊天页，点输入条打字发送。

**语音通话的代码写完了，没有上板验证过。**

### 屏幕长什么样

```
   顶栏（返回 · 标题 · 连接点 · 时钟）
   ┌────────────────────────────────┐
   │  会话 │ 状态 │ 日志            │  ← 底部导航栏
   │       └ chat ─ compose         │  ← 压在上面，没有导航栏
   │       └ call                   │  ← 来电时自己顶上来
   └────────────────────────────────┘
```

| 页 | 文件 | 内容 |
|---|---|---|
| **会话** | [main/ui_convs.c](main/ui_convs.c) | 会话列表，**置顶的在最上面**，其余按最近。每行「名字 + 时间」「最后一句 + 未读角标」，群会话的最后一句带发言人。点一行进聊天页，**长按弹出置顶/免打扰** |
| **聊天** | [main/ui_chat.c](main/ui_chat.c) | 消息气泡，自己的靠右（蓝），对方靠左；群里连续同一个人只在第一条上面写名字；群通知居中显示；有自己视图的类型（图片）自己画。进入即已读。底部一条「输入消息…」，单聊右边还有 📞 |
| **输入** | [main/ui_compose.c](main/ui_compose.c) | 文本框 + 发送键 + 一键短语 + 全键盘。**拼音输入法**：敲 `nihao`，候选栏出「你」「好」。键盘上的 ✓ 等于发送，✗ 等于返回 |
| **通话** | [main/ui_call.c](main/ui_call.c) | 一路通话。来电自己顶上来，返回键是最小化而不是挂断 |
| **状态** | [main/ui_status.c](main/ui_status.c) | WiFi 与信号、IP、账号、长连接已保持多久、收发计数、同步 head、存储、会话数与未读、内存、运行时长 |
| **日志** | [main/ui_log.c](main/ui_log.c) | 收发消息与事件的滚动日志 |

外壳是 [main/ui.c](main/ui.c)，一页一个文件，契约在 [main/ui_page.h](main/ui_page.h)：
一页是 create / refresh / prime / destroy 四个函数，外加零个能活过自己控件的状态。

**置顶和免打扰不是本机开关**，是账号的用户设置（`UG`/`UP`）——在板子上置顶，
手机上那个会话也置顶。所以长按菜单里的按钮只是**记下要什么**，真正发出去在
`prime()` 里：LVGL 回调握着显示锁，而发请求会阻塞在长连接的 socket 上，
这条规矩和发消息是同一条。也因此按下去不会立刻变——服务端确认之后才落本地、
才重画，被拒绝的改动就等于没发生过。

### 消息与视图

聊天页只认两种形状：气泡和居中的通知。别的都是**视图**，按 content type
查表分发，契约在 [main/ui_msg_view.h](main/ui_msg_view.h)。

一个视图就是一个 `draw()`，在 UI 任务上、握着显示锁跑。没有视图的类型由页面画：
注册成通知的居中一行，其余是气泡。只想换一句**话**的类型改 digest 就行——
会话列表和气泡读的是同一个。

**页面给视图的 row 只有信封**：谁、什么时候、是不是自己发的、是不是群，
加一行 digest。没有任何跟类型有关的东西。信封之外的一切按 `message_uid` 去查。

这条线是撞过之后才画的。row 是在 store 的查询回调里填的——那是消息唯一活着的一刻
（字符串指向 sqlite 的行缓冲，下一条 step 进来就失效），所以很想顺手把视图将来要用的
东西抄一份进 row。两个问题：**定长字段会截断**（store 里文本能有 512 字节，
图片 URL 抄进 128 字节的字段就是一张永远加载不出来的图，而且是静默的）；
**一半消息根本没有那个数据**（web 端知道尺寸才写 `{"w","h"}`）。

所以要更多东西的视图**在 `prime()` 里回头读 store**——`wfc_get_message(uid, ...)`，
按 uid 走唯一索引取一条。为此在 client 层补了这个接口：
`wfc_store_has_message(uid)` 早就在了，索引也早就在了，缺的只是「那给我这条」。

不在 `draw()` 里读的原因不是慢。`draw()` **可以**读 store，但它不能阻塞，
而这些字段要拿去干的事（下载图片、打开链接）全都阻塞——所以 `draw()` 拿到答案也没用。
`prime()` 在锁外，能拿答案去干活，而且是有需要才走一次，不是每行走一次。

内置类型的视图在 `main/ui_msg_*.c`，这个部署自己的（≥1000）在
[main/wfc_custom_message/](main/wfc_custom_message/)，**后者先查**——
一个 fork 可以就地换掉内置视图，不用改这边的文件。

图片（type 3）会真的显示出来：[main/ui_media.c](main/ui_media.c) 从 store 取出
`remoteMediaUrl`，HTTP 拉下来，用 TJpgDec 解码成 RGB565（**解的时候就按 1/2~1/8 缩**，
不然一张手机照片解出来是 24 MB），按 `message_uid` 存进 4 个槽里。
图还没到就画一个同样大小的框，所以图落地时页面不会跳。

这件事在**自己的任务上**做，不在 `prime()` 里。`prime()` 跑在 UI 任务上，
而 UI 任务同时也处理导航——下载花四秒，返回键就四秒没反应。

释放只在一个地方发生：`refresh()` 里 `lv_obj_clean()` 之后、重建控件之前
调 `ui_media_keep_only()`。那是唯一没有任何控件持有图片指针的时刻，
而 `ui_media_get()` 交出去的指针 LVGL 会一直拿着。取图的任务只填空槽、从不释放。

### 自定义消息

二次开发最常做的事，单独一个目录：
[main/wfc_custom_message/](main/wfc_custom_message/)，
和 web 端 `vue-chat/src/wfc_custom_message` 五步一一对应，
**不用动 `sdk/` 里的任何东西**。

例子是两个类型：`1001` 普通自定义消息（带标签的气泡，视图表里一行）和
`1002` 自定义通知（注册成通知，居中一行，连视图都不用写）。
在聊天页输入框里发 `/custom` 或 `/tip` 就能在板子上看到。
同一个表还把内置类型的名字换成了中文（「[图片]」「[通话]」）——
组件里不放中文界面串，那是应用的事。

### 读模型反过来了

P4 是 `app_main.c` 订阅事件、查存储、把拼好的字符串推给面板。聊天页做不到这样——
打开一个会话要去读它的消息，而应用层不知道谁什么时候点开了哪个。
所以现在 **UI 自己通过 `wfc_client.h` 读存储**，事件只置一个 dirty 位，
UI 任务下一拍把当前页重画。`app_main.c` 从 681 行掉到 270 行左右。

顺带解决了 P4 的一个真问题：补拉一次投递四十条消息就是四十次重画，
现在 dirty 位在一拍（80 ms）里合并。

### 屏幕上的字换了一套

**LVGL 自带的中文字体画不全常用字，而且 P2~P4 一直是坏的。**
它是照着 LVGL 自己 demo 挑的 1118 个汉字，偏繁体日文。实测：

```
$ python3 tools/check_font.py main/*.c
main/ui.c:83: cannot draw 话  in "会话"
main/ui.c:250: cannot draw 态  in "状态"
...
62 literal(s) the panel would draw with gaps in them.
```

**标签页自己的名字就画不全**——`会话` 画成 `会`。一直没发现，是因为串口日志是对的。

现在是 [components/wfc_font/](components/wfc_font/)：Noto Sans SC（SIL OFL）生成的
GB2312 一级字 3755 个 + ASCII + 中文标点 + LVGL 符号，16 px / 4 bpp，约 480 KB。
生成物进版本库，平常编译不跑生成脚本。字体是画在屏幕上的东西，所以它跟着应用走，
不在 IM 客户端里——那边一行 LVGL 都不碰。

```bash
bash tools/make_font.sh                 # 重新生成字体（要 node + 下载字体）
python3 tools/check_font.py main/*.c    # 加中文字符串之前先跑这个
```

`tools/` 里的三个脚本跟着工程走（路径按脚本自己的位置算，在哪个目录下敲都一样）。

### 中文输入是真能打字的

LVGL 的拼音输入法自带词典是**繁体**的（打 `xie` 出 `謝`），321 个音节，不能用。
[tools/make_pinyin.py](tools/make_pinyin.py) 用 pypinyin + jieba 的词频表
重新生成了 [main/ui_pinyin_dict.c](main/ui_pinyin_dict.c)：405 个音节、
GB2312 一级字全覆盖、**候选按字频排序**（`wo` → 我，`de` → 的，`shi` → 是），17 KB。

```bash
pip install pypinyin jieba && python3 tools/make_pinyin.py
```

还有八个一键短语（你好 / 在 / 收到 / 好的 / 谢谢 / 稍等 / 我在路上了 / 晚点回复你）兜底。

## 音视频是可以裁剪的

聊天页底部输入条右边的 📞 发起呼叫；来电时通话页自己顶上来。
这一整块是 [sdk/wfav](sdk/wfav/)，可以整个不要：

```bash
echo 'CONFIG_APP_CALL=n' >> sdkconfig.local
rm -f sdkconfig && idf.py build
```

| | 固件大小 | ota_0 剩余 |
|---|---|---|
| `CONFIG_APP_CALL=y`（默认） | 4.90 MB | 2% |
| `CONFIG_APP_CALL=n` | 2.37 MB | 53% |

**带通话的固件只剩 2% 余量**，5 MB 的 `ota_0` 基本满了。省下来的 2.5 MB 是整个
WebRTC 栈：esp_peer（ICE/DTLS/SRTP）、esp_capture、Opus 编解码、AFE 回声消除。

做法和 P3 的存储后端是同一套：**一个头文件、两个实现、编译期二选一**——
[main/ui_page.h](main/ui_page.h) 里六个 `ui_call_*` 函数，
`CONFIG_APP_CALL=y` 编 [main/ui_call.c](main/ui_call.c)，
`=n` 编 [main/ui_call_none.c](main/ui_call_none.c)。于是：

- **`main/` 里只有 `ui_call.c` 一个文件 include 了 `wfav.h`**，
  外壳、聊天页、`app_main.c` 一个 `#ifdef` 都没有；
- 关掉之后聊天页不画 📞（`ui_call_available()`），来电没人订阅，
  对呼叫方来说和板子没开机是一样的；
- 别的什么都没变。

再进一步：**把 `sdk/wfav/` 整个删掉**，那它连挂都不挂，
esp_peer / esp_capture 这些托管组件也不会下载。那种情况下 `CONFIG_APP_CALL` 必须是 `n`，
不然编译会停下来告诉你（[main/CMakeLists.txt](main/CMakeLists.txt)）。
两个开关分工不同，是因为 `REQUIRES` 那趟解析读不到 sdkconfig——见下面「坑」的最后一条。

IM 那边为音视频加的几个 API 在 [sdk/wfc/include/wfc_client.h](sdk/wfc/include/wfc_client.h)。

## 编译烧录

先激活 IDF v6.1（每开一个新终端都要），然后：

```bash
. $HOME/esp/esp-idf/export.sh            # 你自己的 IDF 安装位置
cd esp-chat
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

`sdk/` 里的 `.a` 是 **esp32s3 + IDF v6.1** 编出来的，换目标芯片或者换大版本的 IDF
链接会失败——那时候要的是重新打一份包，见 [sdk/README.md](sdk/README.md)。

## 配置

**WiFi 账号和 WFC 测试账号都在 `sdkconfig.local`**（已 gitignore，不进版本库）。
新克隆一份代码后这个文件不存在，编译能过但开机会直接报 “not configured” 退出。
手工建一个：

```bash
cat > sdkconfig.local <<'EOF2'
CONFIG_APP_WIFI_SSID="你的WiFi名"
CONFIG_APP_WIFI_PASSWORD="你的密码"

CONFIG_WFC_USER_ID="..."
CONFIG_WFC_CLIENT_ID="..."
CONFIG_WFC_TOKEN="..."
EOF2
```

⚠️ **token 是和 clientId 绑定的**，两者必须配套。用别人给的测试 token 时
不能让 `CONFIG_WFC_CLIENT_ID` 留空——留空会从 MAC 派生一个（决策 D2），
那个 clientId 认证不过。这是这里最容易踩的坑。

应用自己的配置项在 `menuconfig` 的 **Chat app** 里，定义见
[main/Kconfig.projbuild](main/Kconfig.projbuild)：

| 配置 | 默认 | 说明 |
|---|---|---|
| `CONFIG_APP_CALL` | y | 要不要语音通话，见上面「音视频是可以裁剪的」 |
| `CONFIG_WFC_HOST` / `CONFIG_WFC_ROUTE_PORT` | wildfirechat.net / 80 | IM 服务器 |
| `CONFIG_APP_SNTP_SERVER` / `CONFIG_APP_TIMEZONE` | ntp.aliyun.com / CST-8 | 对时。**对不上就连不上**，加密层有小时数前缀 |
| `CONFIG_APP_TURN_URL` / `_USER` / `_PASSWORD` | 空 | 自建部署的 TURN 服务器。留空就用音视频 SDK 自带的那个（`turn.wildfirechat.net`）。放在应用这一层，是因为 `sdk/` 是二进制、里面的 `CONFIG_WFAV_TURN_*` 拧不动——**这一项会覆盖它** |
| `CONFIG_WFC_PULL_HISTORY` | n | **只在没有存储的 head 时起作用**（首次开机、换了账号、或用 RAM 后端）：从服务端当前位置开始（n），还是从 0 拉漫游历史（y）。有存储的 head 就直接从那儿续，这一项不参与 |

存储相关的配置（RAM / SQLite 后端、消息条数上限……）属于 IM 客户端，
**在这里是编译期定死的**——`sdk/` 是二进制，menuconfig 里没有那个菜单。
打包时的取值见 [sdk/README.md](sdk/README.md)。要改这些得有源码。

唯一一个绕过去了的是 **TURN 服务器**：它是「这块板子属于哪个部署」的问题，
和 `CONFIG_WFC_HOST` 同一类，所以搬到了应用这一层（上表 `CONFIG_APP_TURN_*`），
`main/ui_call.c` 把它传给 `wfav_engine_start()`，压过 SDK 里编死的那个。

生效顺序 `sdkconfig.defaults` → `sdkconfig.local` → `sdkconfig`，后面覆盖前面。
改完前两个之一，**必须删掉 `sdkconfig` 再编译**才生效：

```bash
rm -f sdkconfig && idf.py build
```

## 分区表

[partitions.csv](partitions.csv) 一开始就按**最终形态**切好了：
两个 5 MB 的 OTA 槽 + 5800K 的 FATFS 数据分区。当时一个都用不上，
但改分区表意味着整片擦除重刷，NVS 里的 token 和 SQLite 库都会丢——
提前切到位，P3 直接挂上去就能用。

```
nvs       24K     otadata  8K      phy_init 4K
ota_0     5M      ota_1    5M      storage  5800K (fat，P3 起挂在 /wfc)
```

`storage` 第一次挂载会自动格式化（`format_if_mount_failed`），
数据库是 `/wfc/wfc.db`。

固件大小的走势：P1 到 P2 涨的 520 KB **几乎全是屏幕**；P2 到 P3 涨的 441 KB 全是 SQLite；
P3 到 P4 只涨了 16 KB——协议加业务的代码一共才 33 KB；
P4 到 P5 涨的 500 KB **基本就是字体**（新字体 +480 KB，去掉 LVGL 自带的 −200 KB，
拼音词典 +17 KB，其余是 UI 代码和 LVGL 的键盘/输入法控件）；
**P5 到 P6 涨了 2.5 MB，全是 WebRTC 那一坨**，见上面那张表。

## 坑

- **LVGL 的断言处理器是 `while(1);`**（P5 踩的，也是整个项目到目前为止最难查的一个）。
  表现是**整块屏幕死掉**：不重启、不报错、串口里别的任务照常打日志，
  画面不动、触摸没反应、返回键按不了。
  真实原因只是一句写反了的调用——`lv_ime_pinyin_set_mode()` 断言
  `pinyin_ime->kb != NULL`，而它被写在了 `lv_ime_pinyin_set_keyboard()` 前面。
  LVGL 的 `LV_ASSERT_HANDLER` 默认就是 `while(1);`，所以断言不让程序崩，
  而是把**调用它的那个任务原地钉死**；那个任务正拿着 display lock，
  于是 LVGL 既不能重画也不能读触摸屏。
  `CONFIG_LV_USE_LOG` 默认还是关的，连断言信息都没有。打开之后一行就定位了：
  ```
  [Error] lv_ime_pinyin_set_mode: Asserted at expression: pinyin_ime->kb != NULL
          (NULL pointer) lv_ime_pinyin.c:453
  ```
  **`CONFIG_LV_USE_LOG=y` 已经写进 `sdkconfig.defaults`，不要再关掉。**

- **画中文之前先跑 `tools/check_font.py`**。字体是子集，缺字画出来是空的，
  而串口日志是对的——所以肉眼看日志永远发现不了。

- **组件 `REQUIRES` 里写 `if(CONFIG_...)` 是静默失效的**（P3 在 sqlite3 上踩过，
  P6 拆音视频的时候又踩了一次）。IDF 分两趟处理组件 CMakeLists，`REQUIRES` 在
  **读 sdkconfig 之前**那趟解析，而且是另一个进程，于是条件恒为假。
  `SRCS` 是后一趟算的，可以正常用 `CONFIG_`：**同一个文件里同一个 `if`，
  两个分支行为不一样**。
  所以 [main/CMakeLists.txt](main/CMakeLists.txt) 里 `REQUIRES wfav` 问的是
  「有没有 wfav 可以链」（`sdk/wfav` 或者同级目录里的源码——文件系统，两趟都看得见），
  `SRCS` 问的才是 `CONFIG_APP_CALL`。

- **`ota_0` 只剩 2%**。带通话的固件 4.90 MB，分区 5 MB。再加东西之前先看看
  `idf.py size`，或者认真考虑把分区表改成一个 OTA 槽——但那要整片擦除重刷。
