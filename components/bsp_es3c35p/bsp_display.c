#include "bsp_es3c35p.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77922.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "bsp_display";
#define LCD_HOST SPI2_HOST

#include "st77922_init_cmds.inc"

esp_err_t bsp_display_init(const bsp_display_config_t *cfg,
                           esp_lcd_panel_io_handle_t *io,
                           esp_lcd_panel_handle_t *panel)
{
    spi_bus_config_t bus = ST77922_PANEL_BUS_QSPI_CONFIG(
        BSP_PIN_LCD_SCLK, BSP_PIN_LCD_D0, BSP_PIN_LCD_D1, BSP_PIN_LCD_D2,
        BSP_PIN_LCD_D3, (int)cfg->max_transfer_sz);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg =
        ST77922_PANEL_IO_QSPI_CONFIG(BSP_PIN_LCD_CS, cfg->on_color_done, cfg->user_ctx);
    io_cfg.pclk_hz = cfg->pclk_hz;
    io_cfg.trans_queue_depth = 4;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, io),
                        TAG, "panel io");

    st77922_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
            .supports_madctl_mv = cfg->landscape_mv,
        },
    };
    // The panel shares EN with the ESP32: no reset GPIO, the driver resets by command.
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77922(*io, &panel_cfg, panel), TAG, "new panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "init");
    if (cfg->landscape_mv) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(*panel, true), TAG, "swap_xy");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), TAG, "disp on");
    return ESP_OK;
}

// ---- Backlight -------------------------------------------------------------
#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MODE    LEDC_LOW_SPEED_MODE
#define BL_RES     LEDC_TIMER_10_BIT
#define BL_MAX     ((1 << 10) - 1)

static uint8_t s_brightness;

esp_err_t bsp_backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = BL_MODE, .timer_num = BL_TIMER, .duty_resolution = BL_RES,
        .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "bl timer");
    ledc_channel_config_t c = {
        .gpio_num = BSP_PIN_LCD_BL, .speed_mode = BL_MODE, .channel = BL_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE, .timer_sel = BL_TIMER, .duty = 0, .hpoint = 0,
    };
    return ledc_channel_config(&c);
}

void bsp_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_brightness = percent;
    uint32_t duty = (uint32_t)lroundf(powf(percent / 100.0f, 2.2f) * BL_MAX);
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

uint8_t bsp_backlight_get(void) { return s_brightness; }
