/* 本客户端如何向服务器表明自己的身份。集中在一处，是有意为之。
 *
 *
 * token 是为某个平台签发的，服务器会把平台记在会话上。WFC_PLATFORM 必须与签发
 * 该 token 时使用的平台一致。
 *
 * 一个需要知道的副作用：服务器会让同一账号、同一平台的会话互相顶下线。本设备
 * 声明为 AndroidWearable，因此它抢的是另一台可穿戴客户端，而不是手机或桌面端。
 */

#ifndef WFC_PLATFORM_H
#define WFC_PLATFORM_H

/* 平台号 13，即 AndroidWearable。不能用 WEB(5) 或 WX(6)：那两个走的是上面说的
 * WebSocket 分支。 */
#define WFC_PLATFORM 13

#define WFC_APP_NAME    "cn.wildfirechat.chat"
#define WFC_DEVICE_NAME "esp32s3-box-3b"
#define WFC_PHONE_NAME  "esp32s3-box-3b"
#define WFC_APP_VERSION "0.1"
#define WFC_SDK_VERSION "0.1"

#endif /* WFC_PLATFORM_H */
