#include "os_diag.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_LV_USE_DEMO_BENCHMARK
#include "lvgl.h"
#include "demos/benchmark/lv_demo_benchmark.h"
#endif

static const char *TAG = "diag";

static void heap_line(const char *phase, const char *name, uint32_t caps)
{
    ESP_LOGW(TAG, "MEM %-14s %-8s free=%7u min=%7u largest=%7u", phase, name,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
}

void os_diag_mem(const char *phase)
{
    heap_line(phase, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_line(phase, "dma", MALLOC_CAP_DMA);
    heap_line(phase, "psram", MALLOC_CAP_SPIRAM);
    TaskHandle_t ui = xTaskGetHandle("ui");
    if (ui) {
        ESP_LOGW(TAG, "MEM %-14s ui stack free (high-water) = %u B", phase,
                 (unsigned)uxTaskGetStackHighWaterMark(ui));
    }
}

#if CONFIG_LV_USE_DEMO_BENCHMARK
static void bench_end(const lv_demo_benchmark_summary_t *sum)
{
    for (const lv_demo_benchmark_scene_dsc_t *sc = sum->scenes; sc->create_cb; sc++) {
        // The demo stores sums over `measurement_cnt` samples; the first sample is
        // discarded by the demo itself, which already decremented the counter.
        const uint32_t n = sc->measurement_cnt;
        if (n == 0) continue;
        ESP_LOGW(TAG, "BENCH scene=%-28s fps=%2u cpu=%3u%% render=%3ums flush=%3ums",
                 sc->name, (unsigned)(sc->fps_avg / n), (unsigned)(sc->cpu_avg_usage / n),
                 (unsigned)(sc->render_avg_time / n), (unsigned)(sc->flush_avg_time / n));
    }
    const int n = sum->valid_scene_cnt ? (int)sum->valid_scene_cnt : 1;
    ESP_LOGW(TAG, "BENCH SUMMARY scenes=%d fps=%d cpu=%d%% render=%dms flush=%dms",
             (int)sum->valid_scene_cnt, (int)sum->total_avg_fps / n, (int)sum->total_avg_cpu / n,
             (int)sum->total_avg_render_time / n, (int)sum->total_avg_flush_time / n);
    os_diag_mem("after-bench");
}

void os_diag_run_lvgl_benchmark(void)
{
    lv_demo_benchmark_set_end_cb(bench_end);
    lv_demo_benchmark();
}
#else
void os_diag_run_lvgl_benchmark(void) { ESP_LOGE(TAG, "LVGL benchmark not enabled in this build"); }
#endif
