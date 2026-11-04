#include <cstdarg>
#include <cstdio>

#include "core_internal.h"
#include "driver/temperature_sensor.h"
#include "esp_private/esp_clk.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <climits>
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os_power.h"
#include "os_ui.h"

namespace os {

void logf(const char* tag, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  ESP_LOGI(tag, "%s", buf);
}

}  // namespace os

namespace os::core {

static NvsKv s_sys_kv;

// ---- diagnostics helpers (all state is static: tiny, allocated once, never per call) ----

// Chip temperature sensor, installed lazily on the first request.
static std::mutex s_diag_mu;     // ui and svc tasks may both call sysInfo()
static temperature_sensor_handle_t s_tsens;
static bool s_tsens_tried;

static int16_t chipTempC10() {
  std::lock_guard<std::mutex> lk(s_diag_mu);
  if (!s_tsens_tried) {
    s_tsens_tried = true;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK)
      s_tsens = nullptr;
  }
  float c;
  if (!s_tsens || temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) return INT16_MIN;
  return (int16_t)(c * 10.0f);
}

// Running image and slot sizes, computed once (reads only the image header from flash).
static uint32_t s_app_size, s_slot_size;
static bool s_img_done;

static void imageSizes() {
  if (s_img_done) return;
  s_img_done = true;
  const esp_partition_t* p = esp_ota_get_running_partition();
  if (!p) return;
  s_slot_size = p->size;
  esp_image_metadata_t meta;
  const esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
  if (esp_image_get_metadata(&pos, &meta) == ESP_OK) s_app_size = meta.image_len;
}

// CPU load from the per-core idle task run-time counters. Resampled at most every 500 ms so
// several callers (app + service) do not disturb each other's deltas. No allocation.
static int8_t s_load[2] = {-1, -1};

static void cpuLoad(int8_t out[2]) {
  std::unique_lock<std::mutex> lk(s_diag_mu, std::try_to_lock);
  if (!lk.owns_lock()) { out[0] = s_load[0]; out[1] = s_load[1]; return; }
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && (portNUM_PROCESSORS >= 2)
  static uint32_t last_idle[2], last_total;
  static int64_t last_us;
  const int64_t now = esp_timer_get_time();
  if (last_us == 0 || now - last_us >= 500000) {
    const uint32_t total = (uint32_t)now;       // 1 MHz timer, same base as the RTOS counter
    for (int c = 0; c < 2; c++) {
      TaskStatus_t st;
      TaskHandle_t h = xTaskGetIdleTaskHandleForCore(c);
      if (!h) { s_load[c] = -1; continue; }
      vTaskGetInfo(h, &st, pdFALSE, eRunning);
      if (last_us != 0) {
        const uint32_t dt = total - last_total;
        const uint32_t di = st.ulRunTimeCounter - last_idle[c];
        int busy = dt ? 100 - (int)(((uint64_t)di * 100 + dt / 2) / dt) : 0;
        s_load[c] = (int8_t)(busy < 0 ? 0 : busy > 100 ? 100 : busy);
      }
      last_idle[c] = st.ulRunTimeCounter;
    }
    last_total = total;
    last_us = now;
  }
#endif
  out[0] = s_load[0];
  out[1] = s_load[1];
}

NvsKv& systemKv() { return s_sys_kv; }

class DeviceImpl final : public Device {
 public:
  uint8_t brightness() const override { return os_ui_get_brightness(); }

  void setBrightness(uint8_t percent, bool persist) override {
    os_ui_set_brightness(percent);
    if (persist) s_sys_kv.setU32("bright", os_ui_get_brightness());
  }

  void sysInfo(SysInfo& o) const override {
    o.int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    o.int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    o.int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    o.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    o.psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    o.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    o.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    TaskHandle_t ui = xTaskGetHandle("ui");
    o.ui_stack_free = ui ? uxTaskGetStackHighWaterMark(ui) : 0;
    constexpr uint32_t kInt = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    o.int_total = heap_caps_get_total_size(kInt);
    o.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    constexpr uint32_t kDma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    o.dma_free = heap_caps_get_free_size(kDma);
    o.dma_largest = heap_caps_get_largest_free_block(kDma);
    o.dma_total = heap_caps_get_total_size(kDma);
    imageSizes();
    o.app_size = s_app_size;
    o.app_slot_size = s_slot_size;
    uint32_t fl = 0;
    if (esp_flash_get_size(nullptr, &fl) != ESP_OK) fl = 0;
    o.flash_size = fl;
    o.cpu_mhz = (uint16_t)(esp_clk_cpu_freq() / 1000000);
    o.n_tasks = (uint16_t)uxTaskGetNumberOfTasks();
    o.temp_c10 = chipTempC10();
    cpuLoad(o.cpu_load);
  }

  PowerInfo power() const override {
    os_power_state_t s = os_power_get();
    return PowerInfo{s.vbat_mv, s.percent, s.usb, s.charging};
  }

  uint32_t nowMs() const override { return (uint32_t)(esp_timer_get_time() / 1000); }

  void requestClose() override { closeForegroundAsync(); }
  void sleep() override { requestSleep(); }
};

static DeviceImpl s_device;
Device& device() { return s_device; }

}  // namespace os::core
