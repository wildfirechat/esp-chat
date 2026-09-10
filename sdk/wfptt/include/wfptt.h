/* 对讲 SDK，一个头文件全包含。
 *
 * wfptt 之于 wfc，相当于 ptt.js 之于 WFC.js：一个独立的组件，走同一条连接、
 * 同一套协议，用到的东西 IM 客户端全都已经对外暴露。包含本文件会引入四个头文件
 * —— 管理频道的客户端、由应用提供的音频、界面要订阅的事件，以及三者共用的类型
 * 定义。
 */

#ifndef WFPTT_H
#define WFPTT_H

#include "wfptt_audio.h"
#include "wfptt_client.h"
#include "wfptt_event.h"
#include "wfptt_types.h"

#endif /* WFPTT_H */
