#include "bsp_es3c35p.h"

#include "esp_log.h"

static const char *TAG = "bsp_i2c";
static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t bsp_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BSP_PIN_I2C_SDA,
        .scl_io_num = BSP_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // [MEASURE] board pull-ups; vendor demo enables them
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    s_bus = bus;
    return bus;
}

void bsp_i2c_scan(i2c_master_bus_handle_t bus)
{
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "device found at 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "scan done");
}

i2c_master_bus_handle_t bsp_i2c_get(void) { return s_bus; }
