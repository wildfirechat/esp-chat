# sdk/ —— 二进制形式的 wfc / wfav

这个目录是**生成物**，由内部工作区的 `tools/pack_sdk.py` 从
`wfc-esp` 和 `wfav-esp` 的源码编译打包而来：每个模块一个 `.a` 加一份公开头文件，
是可以直接被 IDF 认出来的组件。工程根目录的 `CMakeLists.txt` **只要看见这个目录
就挂这里**（看不见才去找同级的 `../wfc-esp` 源码），所以内部开发树和这里用的是
同一份 `CMakeLists.txt`——`idf.py build` 开头那行 `wfc: sdk/wfc (packaged .a ...)`
就是它说自己走了哪条路。

| 模块 | 内容 | 归档 | 源码版本 |
|---|---|---|---|
| [wfc/](./wfc/) | IM 客户端：协议、存储、模型、事件 | `lib/libwfc.a` 2786 KB | cc5d0c8 2026-09-03 |
| [wfav/](./wfav/) | 音视频 SDK：通话信令、WebRTC、Opus | `lib/libwfav.a` 214 KB | 85b27ce 2026-09-02 |

```
target      esp32s3
IDF         v6.1.0
打包日期     2026-09-03
sha256
    wfc   c180e83592045ab943d8bf34d9955b10c73f2e6d44f9d6064c34c9f933b6dc71
    wfav  1fdd6363cdeb7e4001872343f6115a231e389fd00c0803395f29b3712facfae9
```

## 公开头文件

- **wfc**：`wfc_client.h`、`wfc_content.h`、`wfc_crypto.h`、`wfc_event.h`、`wfc_mem.h`、`wfc_model.h`、`wfc_mqtt.h`、`wfc_pb.h`、`wfc_platform.h`、`wfc_route.h`、`wfc_store.h`、`wfc_token.h`
- **wfav**：`wfav.h`、`wfav_engine.h`、`wfav_event.h`、`wfav_session.h`、`wfav_types.h`

`pbc`（protobuf 运行时）和 `sqlite3` 的头文件不在这里：它们是 `libwfc.a` 的内部
依赖，已经合进同一个归档，没有任何一个公开头文件 include 它们。

## 编译期已经定死的配置

`.a` 是编译完的产物，所以下面这些选项的值在打包那一刻就固定了，
**menuconfig 里也不会再出现**（Kconfig 没有一起打包，免得给一个拧了不起作用的旋钮）。

| 选项 | 打包时的值 |
|---|---|
| `CONFIG_SQLITE3_BUILD` | `true` |
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

要改这些，得有源码：换成 `../wfc-esp` / `../wfav-esp` 的源码树重新编，
或者让维护者重新打一份包。

**应用传进去的参数不受影响**：`wfc_client_config_t`、`wfav_engine_config_t`
都是运行时结构体，加上整个 [main/Kconfig.projbuild](../main/Kconfig.projbuild)
——服务器地址、账号、要不要通话，那些在这个仓库里是源码。

上表里最值得留意的是 `CONFIG_WFAV_TURN_*`：自建部署换 TURN 服务器**不用重新打包**，
应用这一层有一组 `CONFIG_APP_TURN_URL` / `_USER` / `_PASSWORD`，
[main/ui_call.c](../main/ui_call.c) 把它传给 `wfav_engine_start()`，
压过这里编死的值；留空才用上表的默认。
