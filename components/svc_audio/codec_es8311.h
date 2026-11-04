#pragma once
// Minimal ES8311 DAC driver (playback only) on the new I2C master driver. Slave mode, MCLK
// supplied by the ESP32 at 256 x fs, 16-bit I2S.
#include "driver/i2c_master.h"
#include "esp_err.h"

// Probes the chip id and programs the playback path. Needs MCLK to be running.
esp_err_t codec_es8311_init(i2c_master_bus_handle_t bus);
// Raw DAC volume register: 0x00 = -95.5 dB ... 0xBF = 0 dB (0.5 dB steps).
esp_err_t codec_es8311_set_dac_volume(uint8_t reg);
