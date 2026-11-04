#include "bsp_es3c35p.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch";

#define TP_ADDR         0x55
#define REG_MAX_TOUCHES 0x0009
#define REG_TOUCH_INFO  0x0010
#define REG_POINT0      0x0014
#define POINT_STRIDE    7
#define HW_MAX_POINTS   10

static i2c_master_dev_handle_t s_dev;
static uint8_t s_max_points = 1;

static esp_err_t read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, addr, sizeof addr, buf, len, 50);
}

esp_err_t bsp_touch_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev, &s_dev));

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << BSP_PIN_TP_RST, .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst);
    gpio_config_t irq = {
        .pin_bit_mask = 1ULL << BSP_PIN_TP_INT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&irq);

    gpio_set_level(BSP_PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t n = 0;
    if (read_reg(REG_MAX_TOUCHES, &n, 1) != ESP_OK) {
        ESP_LOGW(TAG, "no answer at 0x%02X", TP_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t mx[2] = {0}, my[2] = {0};
    read_reg(0x0005, mx, 2);
    read_reg(0x0007, my, 2);
    ESP_LOGI(TAG, "max X regs %02X %02X, max Y regs %02X %02X", mx[0], mx[1], my[0], my[1]);
    s_max_points = (n > 0 && n <= HW_MAX_POINTS) ? n : 1;
    ESP_LOGI(TAG, "controller ok, %u points", s_max_points);
    return ESP_OK;
}

int bsp_touch_read(bsp_touch_point_t *pts, int max_pts)
{
    uint8_t info = 0;
    if (read_reg(REG_TOUCH_INFO, &info, 1) != ESP_OK || !(info & 0x08)) {
        return 0;
    }
    uint8_t raw[POINT_STRIDE * HW_MAX_POINTS];
    if (read_reg(REG_POINT0, raw, POINT_STRIDE * s_max_points) != ESP_OK) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < s_max_points && n < max_pts; i++) {
        const uint8_t *p = &raw[i * POINT_STRIDE];
        if (!(p[0] & 0x80)) continue;
        pts[n].x = ((p[0] & 0x3F) << 8) | p[1];
        pts[n].y = ((p[2] & 0x3F) << 8) | p[3];
        n++;
    }
    return n;
}
