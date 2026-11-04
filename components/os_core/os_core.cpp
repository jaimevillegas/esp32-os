#include "os_core.h"

#include "core_internal.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "os_power.h"
#include "os_ui.h"

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
  os_ui_set_brightness_commit_cb(onBrightnessCommit);
  os_power_set_change_cb(onPowerChange);
  startServices();

  os_ui_shell_cfg_t cfg = {.first_screen = buildHome, .tick_hook = busPumpUi};
  ESP_ERROR_CHECK(os_ui_start_shell(&cfg));
  ESP_LOGI(TAG, "up: %u apps", (unsigned)slotCount());
}
