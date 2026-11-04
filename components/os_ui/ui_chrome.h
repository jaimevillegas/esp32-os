#pragma once
// Private to os_ui: persistent UI on lv_layer_top() (status bar + pull-down shade).

#include "lvgl.h"
#include "os_ui.h"

#define UI_BAR_H     OS_UI_BAR_H
#define UI_SHADE_H   240

void ui_status_bar_create(void);
void ui_shade_create(void);

// Closes the shade at once (no animation); used when going to / waking from sleep.
void ui_shade_close_now(void);

// Called by the status bar when the user starts a vertical drag.
void ui_shade_drag_event(lv_event_t *e);
