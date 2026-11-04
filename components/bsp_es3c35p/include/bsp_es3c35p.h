#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Pin map (vendor schematic / demo) ------------------------------------
#define BSP_PIN_LCD_CS    10
#define BSP_PIN_LCD_SCLK  12
#define BSP_PIN_LCD_D0    11
#define BSP_PIN_LCD_D1    13
#define BSP_PIN_LCD_D2    14
#define BSP_PIN_LCD_D3     9
#define BSP_PIN_LCD_BL    41
#define BSP_PIN_I2C_SDA   38
#define BSP_PIN_I2C_SCL   39
#define BSP_PIN_TP_INT    47
#define BSP_PIN_TP_RST    48

// ---- Panel geometry --------------------------------------------------------
// The panel is natively 320x480 (portrait). Landscape is 480x320.
#define BSP_LCD_NATIVE_W  320
#define BSP_LCD_NATIVE_H  480

// ---- Display ---------------------------------------------------------------
typedef struct {
    size_t max_transfer_sz;     // bytes of the largest single draw_bitmap
    uint32_t pclk_hz;           // QSPI clock; start at 40 MHz, raise after the thermal test
    bool landscape_mv;          // plan A: set MADCTL.MV so the panel is 480x320
    esp_lcd_panel_io_color_trans_done_cb_t on_color_done;  // runs in ISR context
    void *user_ctx;
} bsp_display_config_t;

esp_err_t bsp_display_init(const bsp_display_config_t *cfg,
                           esp_lcd_panel_io_handle_t *io,
                           esp_lcd_panel_handle_t *panel);

// ---- Backlight (LEDC, ~20 kHz, gamma-corrected) ----------------------------
esp_err_t bsp_backlight_init(void);
void bsp_backlight_set(uint8_t percent);
uint8_t bsp_backlight_get(void);

// ---- I2C bus ---------------------------------------------------------------
i2c_master_bus_handle_t bsp_i2c_init(void);
// Logs every address that ACKs (answers the H0 question about the I2C map).
void bsp_i2c_scan(i2c_master_bus_handle_t bus);

// ---- Touch (ST77922 TDDI, I2C 0x55) ----------------------------------------
typedef struct {
    uint16_t x;     // raw panel coordinates, portrait 320x480
    uint16_t y;
} bsp_touch_point_t;

esp_err_t bsp_touch_init(i2c_master_bus_handle_t bus);
// Reads one frame. Returns the number of valid points (0 = released).
int bsp_touch_read(bsp_touch_point_t *pts, int max_pts);

#ifdef __cplusplus
}
#endif
