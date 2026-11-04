#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os_core.h"
#include "os_diag.h"
#include "os_ui.h"
#include "sdkconfig.h"

extern "C" void app_main(void) {
  ESP_LOGI("main", "boot");
  os_diag_mem("boot");

#if CONFIG_LV_USE_DEMO_BENCHMARK
  ESP_ERROR_CHECK(os_ui_start(os_diag_run_lvgl_benchmark));     // diag/bench builds: bare LVGL
#else
  os_core_start();
#endif

  vTaskDelay(pdMS_TO_TICKS(1500));
  os_diag_mem("ui-steady");
}
