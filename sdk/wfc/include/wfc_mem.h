/* Where wfc allocations come from.
 *
 * Internal SRAM is the scarce pool on the S3 -- WiFi, TLS and DMA descriptors
 * all have to live there -- while protocol buffers are large, short-lived and
 * touched by the CPU only. So everything here prefers PSRAM and falls back to
 * the internal heap, which also keeps the component usable on a board without
 * PSRAM.
 *
 * Anything these return is released with wfc_free(), which is plain free():
 * IDF's free() dispatches on the heap the pointer came from.
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

/* strdup() into the same heap, because a string copied with strdup() would
 * come from the internal pool and still be released with wfc_free(). NULL in
 * gives NULL out, so a caller that copies an optional string tests the result
 * once rather than twice. */
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
