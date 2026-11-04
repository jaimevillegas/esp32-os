#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Landscape resolution (panel rotated with MADCTL.MV).
#define OS_UI_W 480
#define OS_UI_H 320

// Brings up bus, panel, touch and LVGL, starts the `ui` task and shows the
// hardware test screen. Backlight stays off until the first frame is flushed.
// `screen_cb` builds the first screen before the `ui` task starts (NULL = hardware test screen).
esp_err_t os_ui_start(void (*screen_cb)(void));
void os_ui_test_screen(void);

#ifdef __cplusplus
}
#endif
