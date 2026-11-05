// LVGL allocator that keeps every LVGL object in PSRAM. Internal SRAM is the scarce resource of
// this board; LVGL's many small allocations used to land in it (about 28 KB). Selected with
// CONFIG_LV_USE_CUSTOM_MALLOC.
#include "esp_heap_caps.h"
#include "lvgl.h"

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;                      /* not supported */
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);     /* PSRAM full: fall back */
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon) { (void)mon; }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
