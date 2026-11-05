// Settings: a menu of pages (Display, Wi-Fi, Bluetooth). Each page is in its own file.
#include <cstdio>

#include "page.h"

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
    root_ = root;
    showMenu();
  }

  void onEvent(const os::Msg& m) override {
    if (page_) page_->onEvent(m);
  }

  void onStop() override { closePage(); }

 private:
  struct Entry {
    const char* glyph;
    const char* name;
    Page* (*make)(std::pmr::memory_resource&);
  };
  static constexpr Entry kEntries[3] = {
      {LV_SYMBOL_IMAGE, "Display", makeDisplayPage},
      {LV_SYMBOL_WIFI, "Wi-Fi", makeWifiPage},
      {LV_SYMBOL_BLUETOOTH, "Bluetooth", makeBluetoothPage},
  };

  void clearRoot() {
    closePage();
    lv_obj_clean(root_);
  }

  void closePage() {
    if (!page_) return;
    page_->teardown();
    page_->~Page();
    ctx_->mem().deallocate(page_, 0);
    page_ = nullptr;
  }

  void showMenu() {
    clearRoot();
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root_, 8, 0);
    for (size_t i = 0; i < 3; i++) {
      lv_obj_t* row = lv_obj_create(root_);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, lv_pct(100), 52);
      lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x21, 0x28), 0);
      lv_obj_set_style_bg_color(row, lv_color_make(0x2A, 0x32, 0x3D), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(row, 10, 0);
      lv_obj_set_style_pad_hor(row, 16, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 14, 0);
      lv_obj_set_scrollable(row, false);
      lv_obj_set_clickable(row, true);
      lv_obj_add_event_cb(row, onRow, LV_EVENT_CLICKED, this);
      lv_obj_set_user_data(row, (void*)(uintptr_t)i);

      lv_obj_t* g = lv_label_create(row);
      lv_label_set_text(g, kEntries[i].glyph);
      lv_obj_set_style_text_color(g, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
      lv_obj_t* n = lv_label_create(row);
      lv_label_set_text(n, kEntries[i].name);
      lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_color(n, lv_color_white(), 0);
    }
    lv_obj_t* info = lv_label_create(root_);
    lv_label_set_text_fmt(info, "Settings opened %u time(s)", (unsigned)opens_);
    lv_obj_set_style_text_color(info, lv_palette_main(LV_PALETTE_GREY), 0);
  }

  void openPage(size_t i) {
    clearRoot();
    page_ = kEntries[i].make(ctx_->mem());
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root_, 6, 0);

    lv_obj_t* head = lv_obj_create(root_);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 34);
    lv_obj_set_scrollable(head, false);
    lv_obj_t* back = lv_button_create(head);
    lv_obj_set_size(back, 44, 32);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, this);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_t* t = lv_label_create(head);
    lv_label_set_text(t, page_->title());
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 56, 0);

    lv_obj_t* body = lv_obj_create(root_);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_scrollable(body, false);
    lv_obj_update_layout(body);
    page_->build(body, root_, *ctx_);
  }

  static void onRow(lv_event_t* e) {
    auto* self = static_cast<SettingsApp*>(lv_event_get_user_data(e));
    self->openPage((uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e)));
  }
  static void onBack(lv_event_t* e) {
    // Deleting the widgets while their own event runs is unsafe: defer to the next LVGL cycle.
    lv_async_call(
        [](void* p) { static_cast<SettingsApp*>(p)->showMenu(); }, lv_event_get_user_data(e));
  }

  os::Context* ctx_ = nullptr;
  lv_obj_t* root_ = nullptr;
  Page* page_ = nullptr;
  uint32_t opens_ = 0;
};

constexpr SettingsApp::Entry SettingsApp::kEntries[3];

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "settings",
    .name = "Settings",
    .icon = nullptr,
    .glyph = LV_SYMBOL_SETTINGS,
    .perms = os::NVS | os::NET | os::BLE,
    .psram_arena = 160 * 1024,
    .sram_budget = 0,
    .background = false,
    .make = os::makeApp<SettingsApp>,
    .make_service = nullptr,
};

}  // namespace apps::settings
