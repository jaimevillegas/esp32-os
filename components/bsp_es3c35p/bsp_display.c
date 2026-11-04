#include "bsp_es3c35p.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77922.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "bsp_display";
#define LCD_HOST SPI2_HOST

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

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
    s_io = *io;
    s_panel = *panel;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "init");
    if (cfg->landscape_mv) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(*panel, true), TAG, "swap_xy");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), TAG, "disp on");
    return ESP_OK;
}

// ---- Panel sleep -----------------------------------------------------------
// The esp_lcd_st77922 driver does not implement disp_sleep (esp_lcd_panel_disp_sleep()
// returns ESP_ERR_NOT_SUPPORTED), so the standard MIPI DCS commands are sent through
// the panel IO with the same QSPI framing the driver uses (opcode 0x02, cmd in bits 15:8).
#define DCS_SLPIN   0x10
#define DCS_SLPOUT  0x11
#define DCS_DISPOFF 0x28
#define DCS_DISPON  0x29
#define QSPI_CMD(c) ((0x02u << 24) | ((uint32_t)(c) << 8))

static esp_err_t dcs(uint8_t cmd)
{
    return esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(cmd), NULL, 0);
}

esp_err_t bsp_display_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_io, ESP_ERR_INVALID_STATE, TAG, "display not initialised");
    ESP_RETURN_ON_ERROR(dcs(DCS_DISPOFF), TAG, "DISPOFF");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(dcs(DCS_SLPIN), TAG, "SLPIN");
    vTaskDelay(pdMS_TO_TICKS(120));     // datasheet: no SLPOUT/reset within 120 ms of SLPIN
    return ESP_OK;
}

esp_err_t bsp_display_wake(void)
{
    ESP_RETURN_ON_FALSE(s_io, ESP_ERR_INVALID_STATE, TAG, "display not initialised");
    esp_err_t err = dcs(DCS_SLPOUT);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));     // SLPOUT needs 120 ms before the next command
        err = dcs(DCS_DISPON);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#ifdef BSP_DISPLAY_WAKE_REINIT
    // Fallback if SLPOUT alone does not bring the TDDI panel back on hardware: run the
    // full vendor init sequence again (it contains its own SLPOUT + DISPON).
    err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
#endif
    if (err != ESP_OK) ESP_LOGE(TAG, "panel wake failed: %s", esp_err_to_name(err));
    return err;
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

// Light sleep gates the LEDC clock, so the pin is turned into a plain GPIO driven low
// and latched with a pad hold while the CPU sleeps.
void bsp_backlight_suspend(void)
{
    bsp_backlight_set(0);
    ledc_stop(BL_MODE, BL_CHANNEL, 0);
    gpio_reset_pin(BSP_PIN_LCD_BL);
    gpio_set_direction(BSP_PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_PIN_LCD_BL, 0);
    gpio_hold_en(BSP_PIN_LCD_BL);
}

esp_err_t bsp_backlight_resume(void)
{
    gpio_hold_dis(BSP_PIN_LCD_BL);
    esp_err_t err = bsp_backlight_init();   // re-attaches the pin to LEDC at duty 0
    s_brightness = 0;
    return err;
}

uint8_t bsp_backlight_get(void) { return s_brightness; }
