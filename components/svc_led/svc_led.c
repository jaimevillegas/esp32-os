// Single-wire RGB LED (WS2812 class) on IO40, vendor demo: NEO_GRB + 800 kHz.
// Own tiny RMT encoder (bytes encoder, no DMA, no registry dependency): the only internal SRAM
// cost is the channel and encoder objects.
#include "svc_led.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LED_GPIO 40
#define RMT_RES_HZ 10000000   // 10 MHz: 1 tick = 100 ns

static const char* TAG = "svc_led";
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static SemaphoreHandle_t s_mu;

bool svc_led_init(void) {
  if (s_chan) return true;
  rmt_tx_channel_config_t cc = {
      .gpio_num = LED_GPIO,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = RMT_RES_HZ,
      .mem_block_symbols = 48,       // one block is enough for 24 bits
      .trans_queue_depth = 1,
      .flags.with_dma = 0,
  };
  if (rmt_new_tx_channel(&cc, &s_chan) != ESP_OK) {
    ESP_LOGE(TAG, "rmt channel failed");
    s_chan = NULL;
    return false;
  }
  rmt_bytes_encoder_config_t ec = {
      // WS2812: bit 0 = 0.3 us high / 0.9 us low, bit 1 = 0.9 us high / 0.3 us low, MSB first.
      .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
      .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
      .flags.msb_first = 1,
  };
  s_mu = xSemaphoreCreateMutex();
  if (!s_mu || rmt_new_bytes_encoder(&ec, &s_enc) != ESP_OK || rmt_enable(s_chan) != ESP_OK) {
    ESP_LOGE(TAG, "rmt encoder/enable failed");
    if (s_enc) rmt_del_encoder(s_enc);
    rmt_del_channel(s_chan);
    s_enc = NULL;
    s_chan = NULL;
    return false;
  }
  svc_led_set(0, 0, 0);
  return true;
}

void svc_led_set(uint8_t r, uint8_t g, uint8_t b) {
  if (!s_chan) return;
  const uint8_t grb[3] = {g, r, b};   // the pixel expects green first
  const rmt_transmit_config_t tc = {.loop_count = 0};
  xSemaphoreTake(s_mu, portMAX_DELAY);
  if (rmt_transmit(s_chan, s_enc, grb, sizeof grb, &tc) == ESP_OK) {
    rmt_tx_wait_all_done(s_chan, 20);
    esp_rom_delay_us(60);             // latch: line low for > 50 us before the next frame
  }
  xSemaphoreGive(s_mu);
}
