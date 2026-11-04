#include <cstdarg>
#include <cstdio>

#include "core_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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
