// Manual light sleep, woken by the BOOT button (GPIO0, active low).
//
// requestSleep() may be called from any task. It hands sleepSequence() to the ui task
// (os_ui_request_sleep), which runs it between two LVGL iterations, so LVGL and the
// display are never touched concurrently.
//
// NOTE: during light sleep the USB-Serial-JTAG peripheral is clock-gated, so the USB
// port disconnects (no serial monitor, no flashing) and the host may have to
// re-enumerate it after wake. Nothing is logged while asleep.
#include "core_internal.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os_ui.h"

namespace {

const char* const TAG = "sleep";
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr uint32_t kReleaseTimeoutMs = 5000;   // give up on sleeping if BOOT stays held
constexpr uint32_t kDebounceMs = 40;

bool bootLow() { return gpio_get_level(kBootPin) == 0; }

// Waits until BOOT has been continuously high for kDebounceMs. false on timeout.
bool waitBootReleased(uint32_t timeout_ms) {
  uint32_t stable = 0, waited = 0;
  while (stable < kDebounceMs) {
    if (waited >= timeout_ms) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
    waited += 5;
    stable = bootLow() ? 0 : stable + 5;
  }
  return true;
}

void sleepSequence() {
  using namespace os::core;

  // 1. Audio: stop playback so no I2S/DMA is running when the CPU halts.
  if (os::Audio* a = audioService()) a->stop();

  // 2. BOOT must be released first, otherwise the low level would wake us at once.
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << kBootPin;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
  if (!waitBootReleased(kReleaseTimeoutMs)) {
    ESP_LOGE(TAG, "BOOT held for %u ms, sleep cancelled", (unsigned)kReleaseTimeoutMs);
    return;
  }

  // 3. Display: wait for the last DMA flush, backlight 0, panel SLPIN. Failures are
  //    logged and we still sleep (the panel is dark with the backlight off anyway).
  esp_err_t derr = os_ui_display_suspend();
  if (derr != ESP_OK) ESP_LOGE(TAG, "display suspend: %s", esp_err_to_name(derr));

  // 4. Wake-up source: BOOT low level. Only this source, so no stale timer can wake us.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_err_t err = gpio_wakeup_enable(kBootPin, GPIO_INTR_LOW_LEVEL);
  if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
  // Power domains: RTC peripherals and XTAL stay on AUTO; the digital domain (CPU,
  // internal RAM, peripheral registers) and PSRAM stay powered, so the whole system
  // state, including LVGL, is retained. RTC slow/fast memory is not needed.
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);

  int64_t slept_us = 0;
  if (err == ESP_OK) {
    ESP_LOGW(TAG, "entering light sleep; USB serial disconnects, press BOOT to wake");
    vTaskDelay(pdMS_TO_TICKS(100));   // let the USB-Serial-JTAG FIFO drain
    const int64_t t0 = esp_timer_get_time();
    err = esp_light_sleep_start();    // returns after wake-up
    slept_us = esp_timer_get_time() - t0;
  }
  gpio_wakeup_disable(kBootPin);

  // ---- wake path: nothing below may leave the board dark or stuck ----
  vTaskDelay(pdMS_TO_TICKS(10));      // let the idle tasks run (task watchdog)
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "light sleep failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "woke up (cause %d) after %lld ms", (int)esp_sleep_get_wakeup_cause(),
             (long long)(slept_us / 1000));
  }

  // 5. The wake press must not also trigger anything: wait for BOOT release.
  if (!waitBootReleased(kReleaseTimeoutMs)) ESP_LOGW(TAG, "BOOT still held after wake");

  // 6. Panel + touch wake, shade closed, full redraw; the backlight returns after the
  //    first frame is flushed.
  derr = os_ui_display_resume();
  if (derr != ESP_OK) ESP_LOGE(TAG, "display resume: %s (continuing)", esp_err_to_name(derr));
}

}  // namespace

// Called by the shade's Sleep button (declared weak in os_ui).
extern "C" void os_core_request_sleep(void) { os::core::requestSleep(); }

namespace os::core {

void requestSleep() { os_ui_request_sleep(sleepSequence); }

}  // namespace os::core
