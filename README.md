# esp-chat

野火 IM 在 ESP32-S3-BOX-3 上的聊天客户端：会话与消息、中文输入、联系人与群、单人语音通话和对讲。

## 功能

- **会话**：会话列表、未读角标、置顶 / 免打扰（与账号同步，手机端设置的同样生效）
- **聊天**：文字、图片、语音消息（最长 60 秒）、群通知，自己发出的消息显示已送达 / 已读
- **中文输入**：拼音输入法（GB2312 一级字，候选按字频排序）+ 常用短语
- **联系人**：好友列表、好友请求（同意 / 拒绝）、加好友、删好友、设置备注
- **群**：发起群聊、邀请、移出（群主和管理员）、退出
- **语音通话**：单人语音通话，可在编译时去掉
- **对讲**：按住说话，会话里的人实时收听，可在编译时去掉
- **扫码配网、扫码登录**：新板子开机不需要改代码或配置文件

## 环境

- 硬件：ESP32-S3-BOX-3（16 MB flash + 8 MB PSRAM）
- 软件：ESP-IDF v6.1
- 服务端：野火 IM 服务端和 app server

## 目录结构

```
esp-chat/
├── main/                     应用源码：界面、配网、登录、消息视图
│   └── wfc_custom_message/   自定义消息
├── components/wfc_font/      中文字体
├── sdk/
│   ├── wfc/                  IM 客户端：协议、存储、事件（必需）
│   ├── wfav/                 音视频通话（可选）
│   └── wfptt/                对讲（可选）
├── tools/                    字体、拼音词典的生成脚本和字体检查脚本
├── partitions.csv
└── sdkconfig.defaults
```

`sdk/` 下是预编译好的库，每个模块一个 `.a` 加公开头文件，根目录的
[CMakeLists.txt](CMakeLists.txt) 会自动把它们加入组件路径。这些库针对
**esp32s3 + ESP-IDF v6.1** 编译，换芯片或者换 IDF 大版本会链接失败。
公开头文件列表和编译时已固定的配置见 [sdk/README.md](sdk/README.md)。

二次开发只需要改 `main/`，不需要动 `sdk/`。

## 编译烧录

```bash
. $HOME/esp/esp-idf/export.sh      # 激活 ESP-IDF v6.1，路径换成你自己的安装位置
idf.py build
idf.py -p <串口> flash monitor
```

## 首次开机

一块新板子开机后依次显示两个二维码，都用野火 IM 手机端扫描：

1. **配网**：板子开启热点 `WFChat-XXXXXX`，屏幕上显示二维码和热点密码。
   扫码后手机连上热点，从**板子扫描到的**网络里选一个并输入密码，
   板子连接成功后自动关闭热点。ESP32 只支持 2.4 GHz 网络。
2. **扫码登录**：联网后屏幕显示登录二维码，与 PC 端登录使用同一种码，
   手机扫码并确认登录即可。登录请求发往 `CONFIG_APP_SERVER_URL` 指定的 app server。

WiFi 和账号保存在 NVS 中，之后开机直接连接。要更换网络或账号，
在「我的」页点击 **重新配网** 或 **退出登录**（按两次确认），板子会重启并回到对应的二维码。

### 手工配网

手机端不能扫码时，可以照屏幕上的热点名和密码手工连接热点，再调用板子上的接口
（地址 `192.168.4.1`）：

| 接口 | 说明 |
|---|---|
| `GET /wifi/scan` | 板子扫描到的网络列表 |
| `POST /wifi/config` | 提交 `{"ssid":"...","password":"..."}`，板子开始尝试连接 |
| `GET /wifi/status` | 连接状态：`idle` / `connecting` / `connected` / `failed`，失败时附带原因 |

### 预置配置（调试用）

也可以在工程目录下新建 `sdkconfig.local`（已被 gitignore），把 WiFi 和账号编进固件：

```bash
cat > sdkconfig.local <<'EOF'
CONFIG_APP_WIFI_SSID="你的WiFi名"
CONFIG_APP_WIFI_PASSWORD="你的密码"

CONFIG_WFC_USER_ID="..."
CONFIG_WFC_CLIENT_ID="..."
CONFIG_WFC_TOKEN="..."
EOF
rm -f sdkconfig && idf.py build
```

- 这些值只在**首次开机**时写入 NVS。之后再改 `sdkconfig.local` 不会生效，
  需要先 `idf.py erase-flash` 再烧录。
- **token 与 clientId 绑定**，两者必须配套。`CONFIG_WFC_CLIENT_ID` 留空时会根据 MAC
  地址生成一个 clientId，这时为其他 clientId 签发的 token 无法通过认证。
  扫码登录没有这个问题。

## 配置

应用的配置项在 `idf.py menuconfig` → **Chat app** 中，定义见
[main/Kconfig.projbuild](main/Kconfig.projbuild)：

| 配置 | 默认值 | 说明 |
|---|---|---|
| `CONFIG_APP_CALL` | `y` | 是否包含语音通话，见下文「功能裁剪」 |
| `CONFIG_APP_PTT` | `y` | 是否包含对讲，见下文「功能裁剪」 |
| `CONFIG_APP_SERVER_URL` | `https://app.wildfirechat.net` | app server 地址，扫码登录时使用 |
| `CONFIG_WFC_ROUTE_PORT` | `80` | IM 服务器的 route 端口 |
| `CONFIG_APP_SNTP_SERVER` / `CONFIG_APP_TIMEZONE` | `ntp.aliyun.com` / `CST-8` | 时间同步。时间不准无法连接 IM 服务器 |
| `CONFIG_APP_VOICE_VOLUME` | `80` | 语音消息的播放音量（%） |
| `CONFIG_APP_TURN_URL` / `_USER` / `_PASSWORD` | 空 | 自建的 TURN 服务器，留空则使用 SDK 内置的默认值 |
| `CONFIG_APP_WIFI_SSID` / `_PASSWORD` | 空 | 首次开机写入 NVS 的 WiFi，留空则扫码配网 |
| `CONFIG_WFC_USER_ID` / `_CLIENT_ID` / `_TOKEN` | 空 | 首次开机写入 NVS 的账号，留空则扫码登录 |
| `CONFIG_WFC_PULL_HISTORY` | `y` | 本地没有同步记录时（例如首次登录）是否拉取漫游历史消息 |

IM 服务器地址内置在 `sdk/wfc` 中，不是配置项。
本地存储等 SDK 内部配置已在编译时固定，取值见 [sdk/README.md](sdk/README.md)。

配置的生效顺序是 `sdkconfig.defaults` → `sdkconfig.local` → `sdkconfig`，后者覆盖前者。
修改前两个文件后，需要删掉 `sdkconfig` 重新编译：

```bash
rm -f sdkconfig && idf.py build
```

## 功能裁剪

语音通话和对讲是两个独立的开关，可以分别关闭：

```bash
echo 'CONFIG_APP_CALL=n' >> sdkconfig.local    # 去掉语音通话
echo 'CONFIG_APP_PTT=n'  >> sdkconfig.local    # 去掉对讲
rm -f sdkconfig && idf.py build
```

- 语音通话包含 WebRTC、Opus 编解码和回声消除，约占固件 2.4 MB；关闭后聊天页不再显示 📞。
- 对讲只占十几 KB，且不依赖语音通话，关闭通话后对讲仍然可用。

如果连 SDK 本身都不需要，可以删除 `sdk/wfav/` 或 `sdk/wfptt/`，这样编译时也不会下载
相关依赖组件。删除后对应的开关必须设为 `n`，否则编译会报错并给出提示。

## 自定义消息

扩展消息类型的代码集中在 [main/wfc_custom_message/](main/wfc_custom_message/)，
步骤与野火 Web 端的 `src/wfc_custom_message` 一一对应，详见该目录下的 README。

示例包含两种类型：`1001` 自定义消息（气泡）和 `1002` 自定义通知（居中显示）。
在聊天页输入框里发送 `/custom` 或 `/tip` 可以在板子上看到效果。

## 开发说明

**页面**：外壳是 [main/ui.c](main/ui.c)，每个页面一个文件，接口定义在
[main/ui_page.h](main/ui_page.h)（create / refresh / prime / destroy）。
LVGL 回调持有显示锁，发请求、下载这类会阻塞的操作应放在 `prime()` 中执行。
消息的显示方式按 content type 分发，接口定义在 [main/ui_msg_view.h](main/ui_msg_view.h)。

**中文字体**：[components/wfc_font/](components/wfc_font/) 由 Noto Sans SC 生成，
包含 GB2312 一级字 3755 个、ASCII、中文标点和 LVGL 符号（16 px / 4 bpp）。
字体是子集，缺少的字符会显示为空白，所以在源码中添加中文字符串前请先检查：

```bash
python3 tools/check_font.py main/*.c
```

**重新生成字体和拼音词典**（生成结果已提交到仓库，平常编译不需要运行）：

```bash
bash tools/make_font.sh                                   # 需要 node，会下载字体文件
pip install pypinyin jieba && python3 tools/make_pinyin.py
```

**LVGL 日志**：`sdkconfig.defaults` 中开启了 `CONFIG_LV_USE_LOG`，请不要关闭。
LVGL 断言失败时默认会让当前任务原地停住，界面卡死且没有任何报错，只有开启日志才能看到断言信息。

## 分区表

```
nvs       24K     otadata  8K      phy_init 4K
ota_0     5632K   ota_1    5632K   storage  4992K（FAT，挂载在 /wfc）
```

本地消息数据库位于 `/wfc/wfc.db`，`storage` 分区首次挂载时会自动格式化。

带语音通话的固件已接近 `ota_0` 的容量，添加功能前请先用 `idf.py size` 确认空间。
修改分区表需要整片擦除重新烧录，本地消息数据库以及 NVS 中的 WiFi 和账号都会丢失。

## 第三方资源

- 中文字体 Noto Sans SC，SIL Open Font License 1.1，见
  [components/wfc_font/LICENSE-OFL.txt](components/wfc_font/LICENSE-OFL.txt)
