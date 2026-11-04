#include <cstdio>

#include "os/app.h"

namespace apps::settings {

class SettingsApp final : public os::App {
 public:
  void onCreate(os::Context& ctx) override {
    ctx_ = &ctx;
    uint32_t n = 0;
    ctx.kv().getU32("opens", n);
    opens_ = n + 1;
    ctx.kv().setU32("opens", opens_);           // persisted in this app's NVS namespace
  }

  void onStart(lv_obj_t* root) override {
    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    value_ = lv_label_create(root);
    lv_obj_set_style_text_font(value_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(value_, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(value_, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t* slider = lv_slider_create(root);
    lv_slider_set_range(slider, 5, 100);
    lv_obj_set_size(slider, 440, 24);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_ext_click_area(slider, 16);
    const uint8_t b = ctx_->device().brightness();
    lv_slider_set_value(slider, b, LV_ANIM_OFF);
    show(b);
    lv_obj_add_event_cb(slider, onChange, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider, onRelease, LV_EVENT_RELEASED, this);

    lv_obj_t* info = lv_label_create(root);
    lv_label_set_text_fmt(info, "Settings opened %u time(s)", (unsigned)opens_);
    lv_obj_set_style_text_color(info, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

 private:
  void show(int pct) { lv_label_set_text_fmt(value_, "%d%%", pct); }

  static void onChange(lv_event_t* e) {
    auto* self = static_cast<SettingsApp*>(lv_event_get_user_data(e));
    const int v = lv_slider_get_value(lv_event_get_target_obj(e));
    self->ctx_->device().setBrightness((uint8_t)v, false);   // live preview, no flash write
    self->show(v);
  }
  static void onRelease(lv_event_t* e) {
    auto* self = static_cast<SettingsApp*>(lv_event_get_user_data(e));
    self->ctx_->device().setBrightness(self->ctx_->device().brightness(), true);   // persist once
  }

  os::Context* ctx_ = nullptr;
  lv_obj_t* value_ = nullptr;
  uint32_t opens_ = 0;
};

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "settings",
    .name = "Settings",
    .icon = nullptr,
    .glyph = LV_SYMBOL_SETTINGS,
    .perms = os::NVS,
    .psram_arena = 32 * 1024,
    .sram_budget = 0,
    .background = false,
    .make = os::makeApp<SettingsApp>,
    .make_service = nullptr,
};

}  // namespace apps::settings
