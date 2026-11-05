// Apps page: switch apps on and off. A disabled app disappears from the launcher at once; the
// background Service of a disabled app is not started at the next boot.
#include <cstring>

#include "page.h"

namespace apps::settings {

class AppsPage final : public Page {
 public:
  const char* title() const override { return "Apps"; }

  void build(lv_obj_t* body, lv_obj_t*, os::Context& ctx) override {
    ctx_ = &ctx;
    lv_obj_set_scrollable(body, true);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);

    os::Device& dev = ctx.device();
    for (int i = 0; i < dev.appCount() && n_ < kMax; i++) {
      os::AppInfo a;
      if (!dev.appInfo(i, a)) continue;
      strlcpy(ids_[n_], a.id, sizeof ids_[n_]);

      lv_obj_t* row = lv_obj_create(body);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, lv_pct(100), 38);
      lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x21, 0x28), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(row, 8, 0);
      lv_obj_set_style_pad_hor(row, 12, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 12, 0);
      lv_obj_set_scrollable(row, false);

      lv_obj_t* g = lv_label_create(row);
      lv_label_set_text(g, a.glyph ? a.glyph : LV_SYMBOL_FILE);
      lv_obj_set_style_text_color(g, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
      lv_obj_t* name = lv_label_create(row);
      lv_label_set_text(name, a.name);
      lv_obj_set_style_text_color(name, lv_color_white(), 0);
      lv_obj_set_flex_grow(name, 1);

      lv_obj_t* sw = lv_switch_create(row);
      lv_obj_set_size(sw, 46, 24);
      if (a.enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
      if (!a.can_disable) lv_obj_add_state(sw, LV_STATE_DISABLED);
      lv_obj_add_event_cb(sw, onSwitch, LV_EVENT_VALUE_CHANGED, this);
      lv_obj_set_user_data(sw, (void*)(uintptr_t)n_);
      n_++;
    }

    lv_obj_t* note = lv_label_create(body);
    lv_label_set_text(note, "Disabled apps leave the launcher now; their background services stop "
                            "after a restart.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, lv_pct(100));
    lv_obj_set_style_text_color(note, lv_palette_main(LV_PALETTE_GREY), 0);
  }

 private:
  static constexpr int kMax = 12;

  static void onSwitch(lv_event_t* e) {
    auto* self = static_cast<AppsPage*>(lv_event_get_user_data(e));
    lv_obj_t* sw = lv_event_get_target_obj(e);
    const size_t i = (uintptr_t)lv_obj_get_user_data(sw);
    self->ctx_->device().setAppEnabled(self->ids_[i], lv_obj_has_state(sw, LV_STATE_CHECKED));
  }

  os::Context* ctx_ = nullptr;
  char ids_[kMax][16] = {};
  int n_ = 0;
};

Page* makeAppsPage(std::pmr::memory_resource& m) {
  return new (m.allocate(sizeof(AppsPage), alignof(AppsPage))) AppsPage();
}

}  // namespace apps::settings
