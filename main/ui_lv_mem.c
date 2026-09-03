/* LVGL's allocator, pointed at PSRAM.
 *
 * Internal SRAM is the resource this board runs out of, and it runs out at
 * the worst moment: the AEC's initialisation runs on a task whose 8 KB stack
 * must be internal (esp_capture's CAPTURE_RUN_SYNC_IN_RAM), so a call fails
 * to start with
 *
 *   ESP_GMF_THREAD: Error create task  afe_open in RAM
 *   AUD_AEC_SRC: Failed to open AFE
 *
 * while WiFi, DTLS and the I2S DMA descriptors all hold internal memory.
 *
 * LVGL is the largest movable claim on that pool. Its built-in allocator is a
 * fixed array in .bss -- internal, and reserved whether used or not -- and
 * routing it through the C library instead is not enough on its own, because
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL keeps every allocation under 256 bytes
 * internal, which is most of what a widget tree is made of.
 *
 * So LVGL gets its own rule rather than the global one: everything it
 * allocates goes to PSRAM. Nothing LVGL allocates is touched by DMA -- the
 * panel's draw buffers belong to esp_lvgl_port, which allocates them itself
 * with MALLOC_CAP_DMA -- so the only cost is PSRAM's slower access on widget
 * bookkeeping, which at 80 MHz octal is not visible on a 320x240 UI.
 *
 * The fallback to internal memory matters more than it looks: LVGL's assert
 * handler is `while(1);`, so an allocation that returns NULL does not crash,
 * it parks the calling task -- usually while it holds the display lock, which
 * freezes the panel with no message. Degrading to internal memory is better
 * than that.
 */

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#define LV_MEM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;  /* pools are a built-in-allocator idea; there is no pool */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_MEM_CAPS);

    return p != NULL ? p : malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, LV_MEM_CAPS);

    return n != NULL ? n : realloc(p, new_size);
}

void lv_free_core(void *p)
{
    /* heap_caps_malloc and malloc share one allocator, so this frees either. */
    free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);  /* the figures live in heap_caps_get_info() instead */
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
