#include "os_core.h"

#include "core_internal.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "os_power.h"
#include "os_ui.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

using namespace os;
using namespace os::core;

static const char* TAG = "os_core";

static void initNvs(const char* partition) {
  esp_err_t e = nvs_flash_init_partition(partition);
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase_partition(partition));
    e = nvs_flash_init_partition(partition);
  }
  ESP_ERROR_CHECK(e);
}

static void onBrightnessCommit(uint8_t pct) { systemKv().setU32("bright", pct); }

static void onPowerChange(const os_power_state_t* st) {
  Msg m{};
  m.topic = Topic::Power;
  m.arg = (uint32_t)st->percent | (st->usb ? 0x100u : 0) | (st->charging ? 0x200u : 0);
  bus().publish(m);
}

static void buildHome() { lv_screen_load(launcherBuild()); }

#ifdef CONFIG_OS_DEBUG_RADIOS
// Bring-up measurement aid: turns both radios on at boot and logs the internal heap.
static void logHeap(const char* phase) {
  ESP_LOGW("RADIOS", "%-12s internal free=%u min=%u largest=%u", phase,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
static void debugRadiosTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(4000));
  if (netService()) netService()->setEnabled(false);     // flags are persisted: start from "off"
  if (bleService()) bleService()->setEnabled(false);
  vTaskDelay(pdMS_TO_TICKS(4000));
  logHeap("radios-off");
  if (netService()) netService()->setEnabled(true);
  vTaskDelay(pdMS_TO_TICKS(8000));
  logHeap("wifi-on");
  if (bleService()) bleService()->setEnabled(true);
  vTaskDelay(pdMS_TO_TICKS(8000));
  logHeap("wifi+ble-on");
  vTaskDelay(pdMS_TO_TICKS(8000));
  logHeap("settled");
  if (netService()) netService()->setEnabled(false);     // leave the device with radios off
  if (bleService()) bleService()->setEnabled(false);
  vTaskDelete(nullptr);
}
#endif

extern "C" void os_core_start(void) {
  initNvs("nvs");
  initNvs("nvs_app");

  systemKv().configure("nvs", "cfg");
  uint32_t bright;
  if (systemKv().getU32("bright", bright)) os_ui_preset_brightness((uint8_t)bright);

  busInit();
  managerInit();
  storageStart();
  audioStart();
  netStart();
  bleStart();
  ledStart();
  recorderStart();
  os_ui_set_brightness_commit_cb(onBrightnessCommit);
  os_power_set_change_cb(onPowerChange);
  startServices();

  os_ui_shell_cfg_t cfg = {.first_screen = buildHome, .tick_hook = busPumpUi};
  ESP_ERROR_CHECK(os_ui_start_shell(&cfg));
  ESP_LOGI(TAG, "up: %u apps", (unsigned)slotCount());
#ifdef CONFIG_OS_DEBUG_RADIOS
  xTaskCreate(debugRadiosTask, "dbg_radio", 3072, nullptr, 3, nullptr);
#endif
}
