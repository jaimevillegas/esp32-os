// Placeholder page: replace with the real implementation.
#include "page.h"

namespace apps::settings {

class BluetoothPage final : public Page {
 public:
  const char* title() const override { return "Bluetooth"; }
  void build(lv_obj_t* body, lv_obj_t*, os::Context&) override {
    lv_obj_t* l = lv_label_create(body);
    lv_label_set_text(l, "Bluetooth: under construction");
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
  }
};

Page* makeBluetoothPage(std::pmr::memory_resource& m) {
  return new (m.allocate(sizeof(BluetoothPage), alignof(BluetoothPage))) BluetoothPage();
}

}  // namespace apps::settings
