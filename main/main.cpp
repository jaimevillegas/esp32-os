#include "esp_log.h"
#include "os_ui.h"

extern "C" void app_main(void) {
  ESP_LOGI("main", "boot");
  ESP_ERROR_CHECK(os_ui_start());
}
