// Placeholder app: replace with the real implementation.
#include "os/app.h"

namespace apps::led {

class Stub final : public os::App {
 public:
  void onStart(lv_obj_t* root) override {
    lv_obj_t* l = lv_label_create(root);
    lv_label_set_text(l, "LED: under construction");
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
  }
};

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "led",
    .name = "LED",
    .icon = nullptr,
    .glyph = LV_SYMBOL_EYE_OPEN,
    .perms = os::LED | os::NVS,
    .psram_arena = 131072,
    .sram_budget = 0,
    .background = false,
    .make = os::makeApp<Stub>,
    .make_service = nullptr,
};

}  // namespace apps::led
