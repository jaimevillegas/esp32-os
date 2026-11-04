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
esp_err_t os_ui_start(void);

#ifdef __cplusplus
}
#endif
