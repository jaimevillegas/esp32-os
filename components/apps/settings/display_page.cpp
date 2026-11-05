// Display page: brightness.
#include "page.h"

namespace apps::settings {

class DisplayPage final : public Page {
 public:
  const char* title() const override { return "Display"; }

  void build(lv_obj_t* body, lv_obj_t*, os::Context& ctx) override {
    ctx_ = &ctx;
    lv_obj_t* title = lv_label_create(body);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 4);

    value_ = lv_label_create(body);
    lv_obj_set_style_text_font(value_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(value_, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(value_, LV_ALIGN_TOP_RIGHT, 0, 4);

    lv_obj_t* slider = lv_slider_create(body);
    lv_slider_set_range(slider, 5, 100);
    lv_obj_set_size(slider, 430, 24);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_ext_click_area(slider, 16);
    const uint8_t b = ctx.device().brightness();
    lv_slider_set_value(slider, b, LV_ANIM_OFF);
    show(b);
    lv_obj_add_event_cb(slider, onChange, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider, onRelease, LV_EVENT_RELEASED, this);
  }

 private:
  void show(int pct) { lv_label_set_text_fmt(value_, "%d%%", pct); }
  static void onChange(lv_event_t* e) {
    auto* self = static_cast<DisplayPage*>(lv_event_get_user_data(e));
    const int v = lv_slider_get_value(lv_event_get_target_obj(e));
    self->ctx_->device().setBrightness((uint8_t)v, false);   // live preview, no flash write
    self->show(v);
  }
  static void onRelease(lv_event_t* e) {
    auto* self = static_cast<DisplayPage*>(lv_event_get_user_data(e));
    self->ctx_->device().setBrightness(self->ctx_->device().brightness(), true);   // persist once
  }

  os::Context* ctx_ = nullptr;
  lv_obj_t* value_ = nullptr;
};

Page* makeDisplayPage(std::pmr::memory_resource& m) {
  return new (m.allocate(sizeof(DisplayPage), alignof(DisplayPage))) DisplayPage();
}

}  // namespace apps::settings
