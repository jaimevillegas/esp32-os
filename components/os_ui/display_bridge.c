// LVGL 9 <-> esp_lcd bridge: partial render mode, two internal-SRAM DMA strips.
#include "os_ui.h"

#include "bsp_es3c35p.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "os_ui";

#define STRIP_ROWS   32
#define STRIP_BYTES  (OS_UI_W * STRIP_ROWS * 2)

// Raw touch is portrait 320x480, same orientation as the native panel. LVGL 9
// rotates indev points itself according to the display rotation, so the read
// callback must report PHYSICAL coordinates. The sensor reports roughly 20 units
// of inset at each edge (measured), so the range is scaled to the full panel.
#define TOUCH_RAW_X_MIN  22
#define TOUCH_RAW_X_MAX  302
#define TOUCH_RAW_Y_MIN  18
#define TOUCH_RAW_Y_MAX  455

// Flush benchmark: set to 1 to time full-screen fills before LVGL starts.
#ifndef OS_UI_BENCH
#define OS_UI_BENCH 0
#endif
#ifndef OS_UI_PCLK_HZ
#define OS_UI_PCLK_HZ (80 * 1000 * 1000)
#endif

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static volatile bool s_first_frame;
static uint8_t *s_tx_buf;               // rotated + byte-swapped strip handed to the DMA
static SemaphoreHandle_t s_tx_free;     // given by the ISR when the DMA finished

void os_ui_test_screen(void);

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_free, &woken);
    return woken == pdTRUE;
}

// Plan B: the panel keeps its native 320x480 orientation (MADCTL.MV is ignored by
// this module). LVGL renders landscape strips; here they are rotated 90 degrees and
// byte-swapped into the DMA buffer, then sent to the rotated area.
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    xSemaphoreTake(s_tx_free, portMAX_DELAY);       // previous DMA done
    lv_area_t r = *a;
    lv_display_rotate_area(d, &r);
    lv_draw_sw_rotate(px, s_tx_buf, w, h, w * 2, h * 2,
                      LV_DISPLAY_ROTATION_270, LV_COLOR_FORMAT_RGB565);
    lv_draw_sw_rgb565_swap(s_tx_buf, w * h);
    bool last = lv_display_flush_is_last(d);
    lv_display_flush_ready(d);                      // LVGL may render the next strip now
    esp_lcd_panel_draw_bitmap(s_panel, r.x1, r.y1, r.x2 + 1, r.y2 + 1, s_tx_buf);
    if (last) s_first_frame = true;
}

// The ST77922 needs x_start % 4 == 0 and x_end == 4N+3. Applied to both axes
// until MV is validated on hardware ([MEASURE]).
static void invalidate_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~3;  a->x2 |= 3;
    a->y1 &= ~3;  a->y2 |= 3;
    if (a->x2 > OS_UI_W - 1) a->x2 = OS_UI_W - 1;
    if (a->y2 > OS_UI_H - 1) a->y2 = OS_UI_H - 1;
}

#if OS_UI_BENCH
// Fills the whole native 320x480 panel with a pattern through the same tx buffer
// and semaphore used by the flush path, and logs the mean time per full screen.
static void bench_flush(void)
{
    const int rows = STRIP_BYTES / (BSP_LCD_NATIVE_W * 2);      // 48 rows per strip
    const int runs = 100;
    uint16_t *p = (uint16_t *)s_tx_buf;
    for (int i = 0; i < STRIP_BYTES / 2; i++) p[i] = (i & 1) ? 0xF81F : 0x07E0;
    int64_t t0 = esp_timer_get_time();
    for (int n = 0; n < runs; n++) {
        for (int y = 0; y < BSP_LCD_NATIVE_H; y += rows) {
            xSemaphoreTake(s_tx_free, portMAX_DELAY);
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_NATIVE_W, y + rows, s_tx_buf);
        }
    }
    xSemaphoreTake(s_tx_free, portMAX_DELAY);                   // last DMA finished
    xSemaphoreGive(s_tx_free);
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGW(TAG, "BENCH pclk=%d MHz: full screen %.2f ms (%d runs, %.1f fps max)",
             OS_UI_PCLK_HZ / 1000000, dt / 1000.0 / runs, runs, 1e6 * runs / dt);
}
#endif

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int32_t lx, ly;
    bsp_touch_point_t p;
    if (bsp_touch_read(&p, 1) > 0) {
        ESP_LOGD(TAG, "touch raw x=%u y=%u", p.x, p.y);
        int32_t x = ((int32_t)p.x - TOUCH_RAW_X_MIN) * (BSP_LCD_NATIVE_W - 1) / (TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
        int32_t y = ((int32_t)p.y - TOUCH_RAW_Y_MIN) * (BSP_LCD_NATIVE_H - 1) / (TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);
        x = LV_CLAMP(0, x, BSP_LCD_NATIVE_W - 1);
        y = LV_CLAMP(0, y, BSP_LCD_NATIVE_H - 1);
        lx = x; ly = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = lx;
    data->point.y = ly;
}

static void ui_task(void *arg)
{
    bool bl_on = false;
    for (;;) {
        uint32_t next = lv_timer_handler();
        if (!bl_on && s_first_frame) {          // no garbage flash at power-up
            bsp_backlight_set(80);
            bl_on = true;
        }
        if (next > 50) next = 50;               // TODO H2: block on a notification instead
        vTaskDelay(pdMS_TO_TICKS(next ? next : 1));
    }
}

esp_err_t os_ui_start(void)
{
    ESP_ERROR_CHECK(bsp_backlight_init());
    bsp_backlight_set(0);

    i2c_master_bus_handle_t i2c = bsp_i2c_init();
    bsp_i2c_scan(i2c);

    lv_init();
    lv_tick_set_cb(tick_cb);
    s_tx_free = xSemaphoreCreateBinary();
    xSemaphoreGive(s_tx_free);
    s_disp = lv_display_create(BSP_LCD_NATIVE_W, BSP_LCD_NATIVE_H);
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_270);   // logical 480x320

    esp_lcd_panel_io_handle_t io;
    bsp_display_config_t cfg = {
        .max_transfer_sz = STRIP_BYTES,
        .pclk_hz = OS_UI_PCLK_HZ,
        .landscape_mv = false,
        .on_color_done = on_color_done,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(bsp_display_init(&cfg, &io, &s_panel));

    // One render strip + one transmit strip, both internal SRAM and DMA-capable.
    void *draw = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_tx_buf = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(draw && s_tx_buf, ESP_ERR_NO_MEM, TAG, "draw buffers");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, draw, NULL, STRIP_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);

#if OS_UI_BENCH
    bench_flush();
#endif
    if (bsp_touch_init(i2c) == ESP_OK) {
        lv_indev_t *in = lv_indev_create();
        lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(in, touch_read_cb);
    }

    os_ui_test_screen();

    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 4, NULL, 1);
    return ESP_OK;
}
