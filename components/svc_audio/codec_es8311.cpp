#include "codec_es8311.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8311";
static const uint8_t kAddr = 0x18;

static i2c_master_dev_handle_t s_dev;

static esp_err_t wr(uint8_t reg, uint8_t val) {
    const uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static esp_err_t rd(uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

esp_err_t codec_es8311_init(i2c_master_bus_handle_t bus) {
    if (!bus) return ESP_ERR_INVALID_STATE;
    if (!s_dev) {
        i2c_device_config_t dc = {};
        dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dc.device_address = kAddr;
        dc.scl_speed_hz = 100000;
        esp_err_t e = i2c_master_bus_add_device(bus, &dc, &s_dev);
        if (e != ESP_OK) return e;
    }
    uint8_t id1 = 0, id2 = 0;
    esp_err_t e = rd(0xFD, &id1);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "codec does not answer on 0x%02x", kAddr);
        return e;
    }
    rd(0xFE, &id2);
    ESP_LOGI(TAG, "chip id %02x %02x", id1, id2);

    // Reset and default analog/digital setup (vendor sequence).
    e = wr(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    e |= wr(0x00, 0x00);
    e |= wr(0x00, 0x80);            // power on, slave mode, state machine on
    e |= wr(0x44, 0x08);
    e |= wr(0x44, 0x08);
    e |= wr(0x01, 0x30);
    e |= wr(0x02, 0x00);
    e |= wr(0x03, 0x10);
    e |= wr(0x16, 0x24);
    e |= wr(0x04, 0x10);
    e |= wr(0x05, 0x00);
    e |= wr(0x0B, 0x00);
    e |= wr(0x0C, 0x00);
    e |= wr(0x10, 0x1F);
    e |= wr(0x11, 0x7F);
    e |= wr(0x00, 0x80);
    // Clocks: all on, MCLK from the pin. With MCLK = 256 x fs the dividers are the same for
    // every sample rate: pre_div 1, no multiplier, LRCK = 256 clocks, BCLK = MCLK / 4.
    e |= wr(0x01, 0x3F);
    e |= wr(0x02, 0x00);
    e |= wr(0x03, 0x10);            // ADC OSR
    e |= wr(0x04, 0x10);            // DAC OSR
    e |= wr(0x05, 0x00);
    e |= wr(0x06, 0x03);            // BCLK divider 4
    e |= wr(0x07, 0x00);
    e |= wr(0x08, 0xFF);
    // I2S, 16 bit, not muted.
    e |= wr(0x09, 0x0C);
    e |= wr(0x0A, 0x0C);
    e |= wr(0x13, 0x10);
    e |= wr(0x1B, 0x0A);
    e |= wr(0x1C, 0x6A);
    // Start: power up analog, DAC on, output driver enabled, DAC EQ bypassed.
    e |= wr(0x17, 0xBF);
    e |= wr(0x0E, 0x02);
    e |= wr(0x12, 0x00);
    e |= wr(0x14, 0x1A);
    e |= wr(0x0D, 0x01);
    e |= wr(0x15, 0x40);
    e |= wr(0x37, 0x08);
    e |= wr(0x45, 0x00);
    e |= wr(0x31, 0x00);            // DAC unmuted
    if (e != ESP_OK) ESP_LOGW(TAG, "some register writes failed");
    return e;
}

esp_err_t codec_es8311_set_dac_volume(uint8_t reg) { return wr(0x32, reg); }

// Microphone path, from the vendor echo demo (Example_17_echo, es8311_microphone_config):
//   reg 0x14 = 0x1A  analog mic input selected (bit 6 = 0: no digital/PDM mic), PGA gain 0x0A
//   reg 0x17 = ADC digital volume (vendor 0xC8; default here 0xBF = 0 dB)
//   reg 0x16 = ADC gain scale-up (the init sequence leaves 0x24 = scale 4)
// Registers 0x0D/0x0E (analog + PGA + ADC modulator power-up), 0x15 (ADC ramp), 0x1B/0x1C (ADC
// high-pass, EQ bypass) and the I2S output format (0x0A) are already set by codec_es8311_init().
esp_err_t codec_es8311_enable_capture(void) {
    esp_err_t e = ESP_OK;
    e |= wr(0x0E, 0x02);
    e |= wr(0x14, (uint8_t)(0x10 | (CONFIG_SVC_REC_MIC_PGA & 0x0F)));   // 0x1A at the default
    e |= wr(0x16, (uint8_t)(0x20 | (CONFIG_SVC_REC_ADC_SCALE & 0x07)));
    e |= wr(0x17, (uint8_t)CONFIG_SVC_REC_ADC_VOLUME);
    return e;
}
