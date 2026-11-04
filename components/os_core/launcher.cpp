// Home screen: a grid of tiles, one per app enabled in Kconfig.
#include "core_internal.h"
#include "lvgl.h"
#include "os_ui.h"

namespace os::core {

static lv_obj_t* s_screen;

lv_obj_t* launcherScreen() { return s_screen; }

static void tile_click(lv_event_t* e) {
  launch((size_t)(uintptr_t)lv_event_get_user_data(e));
}

lv_obj_t* launcherBuild() {
  s_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_screen, lv_color_make(0x0E, 0x11, 0x16), 0);
  lv_obj_set_scrollable(s_screen, false);

  lv_obj_t* grid = lv_obj_create(s_screen);
  lv_obj_remove_style_all(grid);
  lv_obj_set_pos(grid, 0, OS_UI_BAR_H);
  lv_obj_set_size(grid, OS_UI_W, OS_UI_H - OS_UI_BAR_H);
  lv_obj_set_style_pad_all(grid, 14, 0);
  lv_obj_set_style_pad_gap(grid, 14, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_scrollable(grid, false);

  for (size_t i = 0; i < slotCount(); i++) {
    const Manifest& m = *slotAt(i)->m;
    lv_obj_t* tile = lv_obj_create(grid);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 104, 100);
    lv_obj_set_style_bg_color(tile, lv_color_make(0x1C, 0x21, 0x28), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_make(0x2A, 0x32, 0x3D), LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, 14, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, 8, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_add_event_cb(tile, tile_click, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

    if (m.icon) {
      lv_obj_t* img = lv_image_create(tile);
      lv_image_set_src(img, m.icon);
    } else {
      lv_obj_t* g = lv_label_create(tile);
      lv_label_set_text(g, m.glyph ? m.glyph : LV_SYMBOL_FILE);
      lv_obj_set_style_text_font(g, &lv_font_montserrat_28, 0);
      lv_obj_set_style_text_color(g, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    }
    lv_obj_t* name = lv_label_create(tile);
    lv_label_set_text(name, m.name);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
  }

  if (slotCount() == 0) {
    lv_obj_t* l = lv_label_create(grid);
    lv_label_set_text(l, "No apps enabled (menuconfig: OS apps)");
  }
  return s_screen;
}

}  // namespace os::core
