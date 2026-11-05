#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Landscape resolution (panel rotated 90/270 by the display bridge).
#define OS_UI_W 480
#define OS_UI_H 320
// Height of the persistent status bar; app content starts below it.
#define OS_UI_BAR_H 32

// Bare start used by the benchmarks and the hardware test: no status bar, no shade.
// `screen_cb` builds the first screen before the `ui` task starts (NULL = hardware test screen).
esp_err_t os_ui_start(void (*screen_cb)(void));
void os_ui_test_screen(void);

// Full shell: theme, power monitor, status bar and pull-down shade.
typedef struct {
    void (*first_screen)(void);   // builds the first screen (runs before the ui task starts)
    void (*tick_hook)(void);      // runs inside the ui task on every iteration
} os_ui_shell_cfg_t;
esp_err_t os_ui_start_shell(const os_ui_shell_cfg_t *cfg);

// Light sleep support. `request_sleep` is callable from any task: it stores `sequence`
// and wakes the ui task, which runs it (so LVGL is never touched concurrently). The
// sequence calls suspend before esp_light_sleep_start() and resume after it.
// suspend: waits for the last DMA flush, backlight 0, panel SLPIN.
// resume: panel SLPOUT, touch check, full redraw; the backlight returns after the first
// frame is flushed. Both log failures, return the first error and never abort half way.
void os_ui_request_sleep(void (*sequence)(void));
esp_err_t os_ui_display_suspend(void);
esp_err_t os_ui_display_resume(void);

// Wakes the ui task (e.g. after posting an event it must handle).
void os_ui_wake(void);

// Brightness in percent (5..100). `preset` only stores the value (call before os_ui_start*);
// `set` also applies it. `commit_cb` fires when the user releases the shade slider.
uint8_t os_ui_get_brightness(void);
void os_ui_preset_brightness(uint8_t pct);
void os_ui_set_brightness(uint8_t pct);
void os_ui_set_brightness_commit_cb(void (*cb)(uint8_t pct));

// Shows/hides the microSD icon in the status bar. Thread-safe (just sets a flag).
void os_ui_set_sd_present(bool present);

// Status bar radio icons. Thread-safe (just set flags, drawn by the 1 s refresh).
// wifi: -1 = hidden (radio off), 0 = on but not connected, 1..3 = connected, signal bars.
// bt:   0 = hidden (stack off), 1 = on, 2 = a device is connected.
void os_ui_set_wifi(int level);
void os_ui_set_bt(int state);

// Status bar title. With a `back_cb` the title shows a back chevron and is tappable.
void os_ui_set_title(const char *name, void (*back_cb)(void));

// Media card of the pull-down shade. os_ui does not know the audio engine: the owner of the
// engine registers these callbacks (all run in the ui task). `get` is polled (~4 Hz) only
// while the shade is visible; the card is shown when `active` (playing or paused).
typedef struct {
    bool active;
    bool playing;
    char title[64];
    uint32_t pos_ms, dur_ms;     // dur_ms 0 = unknown
    uint8_t volume;              // 0..100
} os_ui_media_t;

typedef struct {
    void (*get)(os_ui_media_t *out);
    void (*prev)(void);
    void (*toggle)(void);        // pause / resume
    void (*next)(void);
    void (*set_volume)(uint8_t pct);
} os_ui_media_cfg_t;

// Registers the provider (the struct is copied). NULL removes it and hides the card.
void os_ui_set_media_provider(const os_ui_media_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
