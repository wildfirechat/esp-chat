# sdk/ —— 二进制形式的 wfc / wfav / wfptt

这个目录是**生成物**，由内部工作区的 `tools/pack_sdk.py` 从
`wfc-esp`、`wfav-esp`、`wfptt-esp` 的源码编译打包而来：每个模块一个 `.a` 加一份公开头文件，
是可以直接被 IDF 认出来的组件。工程根目录的 `CMakeLists.txt` **只要看见这个目录
就挂这里**（看不见才去找同级的 `../wfc-esp` 源码），所以内部开发树和这里用的是
同一份 `CMakeLists.txt`——`idf.py build` 开头那行 `wfc: sdk/wfc (packaged .a ...)`
就是它说自己走了哪条路。

| 模块 | 内容 | 归档 | 源码版本 |
|---|---|---|---|
| [wfc/](./wfc/) | IM 客户端：协议、存储、模型、事件 | `lib/libwfc.a` 2980 KB | 4bfb4d1 2026-09-09 |
| [wfav/](./wfav/) | 音视频 SDK：通话信令、WebRTC、Opus | `lib/libwfav.a` 214 KB | 6cf1f8e 2026-09-09 |
| [wfptt/](./wfptt/) | 对讲 SDK：频道、抢麦、AMR-NB 分片 | `lib/libwfptt.a` 67 KB | fc112db 2026-09-09 |

```
target      esp32s3
IDF         v6.1.0
打包日期     2026-09-10
sha256
    wfc   e8141f757ab837138607741b9a8c2da165d07957357ac4c4e70fe178455c3723
    wfav  d9267ef4be075b6dc49391ae67e2bb00f80d8d35d7edf113b9da1ecb78ef89d7
    wfptt 1a063cfaf50bae9ca2cb8b020352deb92325492ec704e653d4b0503841b0c837
```

## 公开头文件

- **wfc**：`wfc_client.h`、`wfc_content.h`、`wfc_event.h`、`wfc_media.h`、`wfc_mem.h`、`wfc_model.h`、`wfc_platform.h`、`wfc_store.h`
- **wfav**：`wfav.h`、`wfav_engine.h`、`wfav_event.h`、`wfav_session.h`、`wfav_types.h`
- **wfptt**：`wfptt.h`、`wfptt_audio.h`、`wfptt_client.h`、`wfptt_event.h`、`wfptt_types.h`

`pbc`（protobuf 运行时）和 `sqlite3` 的头文件不在这里：它们是 `libwfc.a` 的内部
依赖，已经合进同一个归档，没有任何一个公开头文件 include 它们。

模块内部的头文件也不在这里。它们是 SDK 和服务器之间的那一层——长连接、`/route`、
protobuf 读写、报文加解密——已经编进 `.a`，应用既不需要 include，也不应该依赖：
它们随协议变动，不算对外接口。

- **wfc**：`wfc_crypto.h`、`wfc_mqtt.h`、`wfc_pb.h`、`wfc_route.h`、`wfc_token.h`

## 编译期已经定死的配置

`.a` 是编译完的产物，所以下面这些选项的值在打包那一刻就固定了，
**menuconfig 里也不会再出现**（Kconfig 没有一起打包，免得给一个拧了不起作用的旋钮）。

| 选项 | 打包时的值 |
|---|---|
| `CONFIG_SQLITE3_BUILD` | `true` |
| `CONFIG_SQLITE3_ENCRYPT` | `true` |
| `CONFIG_WFAV_AUDIO_AEC` | `true` |
| `CONFIG_WFAV_AUDIO_CHANNELS` | `1` |
| `CONFIG_WFAV_AUDIO_SAMPLE_RATE` | `16000` |
| `CONFIG_WFAV_FORCE_RELAY` | `false` |
| `CONFIG_WFAV_MIC_GAIN_DB` | `37` |
| `CONFIG_WFAV_MIC_LOOPBACK` | `false` |
| `CONFIG_WFAV_RING_TIMEOUT_S` | `60` |
| `CONFIG_WFAV_TURN_PASSWORD` | `wfchatpwd` |
| `CONFIG_WFAV_TURN_URL` | `turn:turn.wildfirechat.net:3478` |
| `CONFIG_WFAV_TURN_USER` | `wfchat` |
| `CONFIG_WFC_STORE_ENCRYPT` | `true` |
| `CONFIG_WFC_STORE_MAX_CONVERSATIONS` | `64` |
| `CONFIG_WFC_STORE_MAX_DATA` | `1024` |
| `CONFIG_WFC_STORE_MAX_GROUP_MEMBERS` | `128` |
| `CONFIG_WFC_STORE_MAX_MESSAGES` | `1000` |
| `CONFIG_WFC_STORE_MAX_PROFILES` | `256` |
| `CONFIG_WFC_STORE_MAX_TEXT` | `512` |
| `CONFIG_WFC_STORE_MOUNT_POINT` | `/wfc` |
| `CONFIG_WFC_STORE_PARTITION` | `storage` |
| `CONFIG_WFC_STORE_RAM` | `false` |
| `CONFIG_WFC_STORE_SQLITE` | `true` |
| `CONFIG_WFPTT_CHUNK_MS` | `400` |
| `CONFIG_WFPTT_GROUP_MAX_SPEAKERS` | `3` |
| `CONFIG_WFPTT_LOCK_SECONDS` | `5` |
| `CONFIG_WFPTT_MAX_TALKERS` | `8` |
| `CONFIG_WFPTT_MAX_TALK_SECONDS` | `60` |
| `CONFIG_WFPTT_PLAY_QUEUE` | `8` |
| `CONFIG_WFPTT_SAVE_VOICE_MESSAGE` | `true` |
| `CONFIG_WFPTT_SINGLE_MAX_SPEAKERS` | `1` |
| `CONFIG_WFPTT_STALE_MS` | `60000` |
| `CONFIG_WFPTT_TALKER_TIMEOUT_MS` | `2000` |

要改这些，得有源码：换成 `../wfc-esp` / `../wfav-esp` 的源码树重新编，
或者让维护者重新打一份包。

**应用传进去的参数不受影响**：`wfc_client_config_t`、`wfav_engine_config_t`
都是运行时结构体，加上整个 [main/Kconfig.projbuild](../main/Kconfig.projbuild)
——服务器地址、账号、要不要通话，那些在这个仓库里是源码。

上表里最值得留意的是 `CONFIG_WFAV_TURN_*`：自建部署换 TURN 服务器**不用重新打包**，
应用这一层有一组 `CONFIG_APP_TURN_URL` / `_USER` / `_PASSWORD`，
[main/ui_call.c](../main/ui_call.c) 把它传给 `wfav_engine_start()`，
压过这里编死的值；留空才用上表的默认。
