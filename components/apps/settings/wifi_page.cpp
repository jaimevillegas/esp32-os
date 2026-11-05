// Placeholder page: replace with the real implementation.
#include "page.h"

namespace apps::settings {

class WifiPage final : public Page {
 public:
  const char* title() const override { return "Wi-Fi"; }
  void build(lv_obj_t* body, lv_obj_t*, os::Context&) override {
    lv_obj_t* l = lv_label_create(body);
    lv_label_set_text(l, "Wi-Fi: under construction");
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
  }
};

Page* makeWifiPage(std::pmr::memory_resource& m) {
  return new (m.allocate(sizeof(WifiPage), alignof(WifiPage))) WifiPage();
}

}  // namespace apps::settings
