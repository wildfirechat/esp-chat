# wfc_font —— 屏幕上的字

`wfc_font_16` 是用 `tools/make_font.sh` 生成的，源字体 **Noto Sans SC**
（SIL Open Font License 1.1，见 [LICENSE-OFL.txt](LICENSE-OFL.txt)）。

## 为什么不用 LVGL 自带的思源黑体

LVGL 的 `lv_font_source_han_sans_sc_16_cjk` 是**给 LVGL 自己的 demo 配的固定子集**，
约 1100 个汉字，而且偏繁体和日文。子集外的字**不是画成方块，是什么都不画**——
一个字的位置上就是一段空白。

P2~P4 一直用的是它。实测结果是：

```
$ python3 tools/check_font.py main/*.c
main/ui.c:83: cannot draw 话  in "会话"
main/ui.c:250: cannot draw 态  in "状态"
main/app_main.c:106: cannot draw 连  in "WiFi 已连接"
...
62 literal(s) the panel would draw with gaps in them.
```

**标签页的名字自己就画不全**，而串口日志一直是对的，所以没人发现。
状态面板还能忍，聊天界面不能——聊天界面上的字是别人在手机上打的，
不在我们的控制范围内。

## 里面有什么

| | 数量 | 为什么 |
|---|---|---|
| ASCII `0x20-0x7F` | 96 | |
| GB2312 一级字 | 3755 | 覆盖现代简体中文的绝大部分。二级字（3008 个）是生僻字，翻一倍的 flash 换聊天里基本不会出现的字，不要 |
| 中文标点 | 30 | `，。？！` 这些**不在** GB2312 一级字区里，但每句话都有 |
| 几何符号 | 16 | `● ← → ✓`，当文字画比当控件画简单 |
| FontAwesome | 61 | `LV_SYMBOL_*`。**键盘控件的按键表就是拿它拼的**，漏了不是少个图标，是键盘不能用 |

合计约 3800 个字形，16 px / 4 bpp，**约 480 KB flash**。

选 4 bpp 不选 2 bpp：16 px 的汉字笔画只有一个像素宽，2 bpp 的抗锯齿会让一屏字
糊成灰噪点。代价是字体大一倍，而 5 MB 的 app 分区付得起。

**只有一个字号**。再来一个字号就是再来 480 KB，而字号想表达的东西，
颜色和位置基本都能表达。确实需要小号拉丁字符的地方（时间戳、计数）用
`lv_font_montserrat_12`，LVGL 自带，不额外占空间。

## 还是画不出来的字

GB2312 二级字（生僻字，多见于人名）、繁体、假名、emoji。

所以 [tools/check_font.py](../../tools/check_font.py) 得留着——
它把源码里所有字符串字面量拿去跟字体的字符集对一遍，
把画不出来的字**在编译前**指出来，而不是等板子摆到桌上才发现。

```bash
python3 tools/check_font.py main/*.c        # 在工程目录里跑
```

消息正文里的生僻字仍然会漏，那要么上二级字（再 +380 KB），
要么走 FreeType/tiny_ttf 从 FATFS 上的字体文件里现取字形（见 ASSESSMENT.md §8.7）。

## 重新生成

```bash
bash tools/make_font.sh
```

需要 node 和一次 8 MB 的字体下载，所以**生成物是进版本库的**，
平常编译不会跑它。
