/* wfc 内部的内存分配。
 *
 * 内部 SRAM 在 S3 上是稀缺资源 —— WiFi、TLS 和 DMA 描述符都得住在那里 ——
 * 而协议缓冲区又大、生命周期短、且只被 CPU 访问。所以这里的分配一律优先用
 * PSRAM，失败再退回内部堆，这也使本组件在没有 PSRAM 的板子上照样可用。
 *
 * 这些函数返回的内存用 wfc_free() 释放，它就是 free()：IDF 的 free() 会按指针
 * 所属的堆来分派。
 */

#ifndef WFC_MEM_H
#define WFC_MEM_H

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void *wfc_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void *wfc_calloc(size_t count, size_t size)
{
    return heap_caps_calloc_prefer(count, size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void wfc_free(void *p)
{
    free(p);
}

/* 复制字符串到同一个堆上。用 strdup() 复制出来的内存来自内部堆，却同样会被
 * wfc_free() 释放，所以这里提供一个对应的版本。传入 NULL 返回 NULL，这样复制
 * 可选字符串的调用方只需判断一次结果。 */
static inline char *wfc_strdup(const char *text)
{
    if (text == NULL) {
        return NULL;
    }

    size_t size = strlen(text) + 1;
    char  *copy = wfc_malloc(size);

    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

#ifdef __cplusplus
}
#endif

#endif /* WFC_MEM_H */
