// LVGL 9 <-> esp_lcd bridge: partial render mode. The render strip can live in PSRAM;
// it is rotated and sent in chunks through two small internal-SRAM DMA buffers.
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
#include "os_power.h"
#include "ui_chrome.h"

static const char *TAG = "os_ui";

#define STRIP_ROWS   CONFIG_OS_UI_STRIP_ROWS
#define STRIP_BYTES  (OS_UI_W * STRIP_ROWS * 2)
#define TX_BYTES     CONFIG_OS_UI_TX_CHUNK_BYTES

// Raw touch is portrait 320x480, same orientation as the native panel. LVGL 9
// rotates indev points itself according to the display rotation, so the read
// callback must report PHYSICAL coordinates. The sensor reports roughly 20 units
// of inset at each edge (measured), so the range is scaled to the full panel.
#define TOUCH_RAW_X_MIN  22
#define TOUCH_RAW_X_MAX  302
#define TOUCH_RAW_Y_MIN  18
#define TOUCH_RAW_Y_MAX  455

#define OS_UI_PCLK_HZ (CONFIG_OS_UI_PCLK_MHZ * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static volatile bool s_first_frame;
static TaskHandle_t s_ui_task;
static void (*s_tick_hook)(void);
static uint8_t *s_tx_buf[2];            // rotated + byte-swapped chunks handed to the DMA
static SemaphoreHandle_t s_tx_free;     // counting (2): one token per free tx buffer, given by the ISR


static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_free, &woken);
    return woken == pdTRUE;
}

// Plan B: the panel keeps its native 320x480 orientation (MADCTL.MV is ignored by
// this module). LVGL renders landscape strips; here every strip is cut into column
// slices that fit a tx buffer, each slice is rotated and byte-swapped into the buffer
// and sent to its rotated area. DMA completes in order, so alternating two buffers
// with a two-token semaphore is enough to know when a buffer is free again.
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const int w = lv_area_get_width(a), h = lv_area_get_height(a);
    int slice_w = (TX_BYTES / (h * 2)) & ~3;        // keep slices 4-pixel aligned
    if (slice_w < 4) slice_w = 4;
    static unsigned n;

    for (int cx = 0; cx < w; cx += slice_w) {
        const int cw = LV_MIN(slice_w, w - cx);
        uint8_t *buf = s_tx_buf[n++ & 1];
        xSemaphoreTake(s_tx_free, portMAX_DELAY);   // this buffer's previous DMA is done
        lv_area_t r = { a->x1 + cx, a->y1, a->x1 + cx + cw - 1, a->y2 };
        lv_display_rotate_area(d, &r);
        lv_draw_sw_rotate(px + cx * 2, buf, cw, h, w * 2, h * 2,
                          LV_DISPLAY_ROTATION_270, LV_COLOR_FORMAT_RGB565);
        lv_draw_sw_rgb565_swap(buf, cw * h);
        if (cx + cw >= w) {                         // whole strip consumed: LVGL may reuse it
            if (lv_display_flush_is_last(d)) s_first_frame = true;
            lv_display_flush_ready(d);
        }
        esp_lcd_panel_draw_bitmap(s_panel, r.x1, r.y1, r.x2 + 1, r.y2 + 1, buf);
    }
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

#ifdef CONFIG_OS_UI_FLUSH_BENCH
// Fills the whole native 320x480 panel through the same tx buffers and semaphore
// used by the flush path, and logs the mean time per full screen.
static void bench_flush(void)
{
    const int rows = (TX_BYTES / (BSP_LCD_NATIVE_W * 2)) & ~3;
    const int runs = 100;
    for (int b = 0; b < 2; b++) {
        uint16_t *p = (uint16_t *)s_tx_buf[b];
        for (int i = 0; i < TX_BYTES / 2; i++) p[i] = (i & 1) ? 0xF81F : 0x07E0;
    }
    unsigned n = 0;
    int64_t t0 = esp_timer_get_time();
    for (int r = 0; r < runs; r++) {
        for (int y = 0; y < BSP_LCD_NATIVE_H; y += rows) {
            int ye = LV_MIN(y + rows, BSP_LCD_NATIVE_H);
            xSemaphoreTake(s_tx_free, portMAX_DELAY);
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_NATIVE_W, ye, s_tx_buf[n++ & 1]);
        }
    }
    xSemaphoreTake(s_tx_free, portMAX_DELAY);                   // drain both buffers
    xSemaphoreTake(s_tx_free, portMAX_DELAY);
    xSemaphoreGive(s_tx_free);
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

void os_ui_wake(void)
{
    if (s_ui_task) xTaskNotifyGive(s_ui_task);
}

static void ui_task(void *arg)
{
    bool bl_on = false;
    for (;;) {
        if (s_tick_hook) s_tick_hook();
        uint32_t next = lv_timer_handler();
        if (!bl_on && s_first_frame) {          // no garbage flash at power-up
            bsp_backlight_set(os_ui_get_brightness());
            bl_on = true;
        }
        if (next > 50) next = 50;
        if (next == 0) next = 1;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next));   // sleeps until the next timer or os_ui_wake()
    }
}

static esp_err_t ui_start(void (*screen_cb)(void), const os_ui_shell_cfg_t *shell)
{
    ESP_ERROR_CHECK(bsp_backlight_init());
    bsp_backlight_set(0);

    i2c_master_bus_handle_t i2c = bsp_i2c_init();
    bsp_i2c_scan(i2c);

    lv_init();
    lv_tick_set_cb(tick_cb);
    s_tx_free = xSemaphoreCreateCounting(2, 2);
    s_disp = lv_display_create(BSP_LCD_NATIVE_W, BSP_LCD_NATIVE_H);
    lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_270);   // logical 480x320

    esp_lcd_panel_io_handle_t io;
    bsp_display_config_t cfg = {
        .max_transfer_sz = TX_BYTES,
        .pclk_hz = OS_UI_PCLK_HZ,
        .landscape_mv = false,
        .on_color_done = on_color_done,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(bsp_display_init(&cfg, &io, &s_panel));

    // Render strip (PSRAM or SRAM) + two small transmit buffers (internal SRAM, DMA).
#ifdef CONFIG_OS_UI_BUF_PSRAM
    void *draw = heap_caps_aligned_alloc(64, STRIP_BYTES, MALLOC_CAP_SPIRAM);
#else
    void *draw = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#endif
    for (int i = 0; i < 2; i++) {
        s_tx_buf[i] = heap_caps_malloc(TX_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    ESP_RETURN_ON_FALSE(draw && s_tx_buf[0] && s_tx_buf[1], ESP_ERR_NO_MEM, TAG, "draw buffers");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, draw, NULL, STRIP_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);

#ifdef CONFIG_OS_UI_FLUSH_BENCH
    bench_flush();
#endif
    if (bsp_touch_init(i2c) == ESP_OK) {
        lv_indev_t *in = lv_indev_create();
        lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(in, touch_read_cb);
    }

    if (shell) {
        lv_theme_default_init(s_disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                              true, LV_FONT_DEFAULT);
        os_power_start();
        if (shell->first_screen) shell->first_screen();
        ui_status_bar_create();
        ui_shade_create();
        s_tick_hook = shell->tick_hook;
    } else {
        (screen_cb ? screen_cb : os_ui_test_screen)();
    }

    xTaskCreatePinnedToCore(ui_task, "ui", 12288, NULL, 4, &s_ui_task, 1);
#ifdef CONFIG_OS_UI_BUF_PSRAM
    const char *where = "PSRAM";
#else
    const char *where = "SRAM";
#endif
    ESP_LOGI(TAG, "ui up: strip %d rows (%d B) in %s, 2 x %d B tx buffers in SRAM, pclk %d MHz",
             STRIP_ROWS, STRIP_BYTES, where, TX_BYTES, CONFIG_OS_UI_PCLK_MHZ);
    return ESP_OK;
}

esp_err_t os_ui_start(void (*screen_cb)(void)) { return ui_start(screen_cb, NULL); }
esp_err_t os_ui_start_shell(const os_ui_shell_cfg_t *cfg) { return ui_start(NULL, cfg); }
