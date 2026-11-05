// RGB LED app. The Service (persistent arena, `svc` task) owns the LED state, runs the effects
// and persists the settings, so effects keep running while the UI is closed. The UI only edits
// the shared state and sends a wake-up message; it never touches the LED itself.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "os/app.h"

namespace apps::led {

namespace {

// Private bus topic: UI -> service. arg bit 0 = "commit" (persist the settings).
constexpr os::Topic kTopicLed = static_cast<os::Topic>(0x14C);

enum Effect : int { kSolid = 0, kBreathe, kBlink, kRainbow, kEffectCount };

struct Shared {
  std::atomic<int> on{0};
  std::atomic<int> hue{200};        // 0..359
  std::atomic<int> sat{100};        // 0..100
  std::atomic<int> bright{40};      // 1..100
  std::atomic<int> effect{kSolid};
  std::atomic<int> speed{50};       // 1..100
  std::atomic<uint32_t> shown{0};   // 0xRRGGBB last sent to the LED (live preview for the UI)
};

Shared* g_sh = nullptr;             // lives inside the Service (persistent arena)

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// HSV -> RGB, h 0..359, s 0..100, v 0..255.
uint32_t hsv(int h, int s, int v) {
  h = ((h % 360) + 360) % 360;
  const int region = h / 60, rem = h % 60;
  const int p = v * (100 - s) / 100;
  const int q = v * (100 - s * rem / 60) / 100;
  const int t = v * (100 - s * (60 - rem) / 60) / 100;
  int r, g, b;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ---------------------------------------------------------------------------------------------
class LedService final : public os::Service {
 public:
  void onCreate(os::Context& ctx) override {
    led_ = ctx.led();
    kv_ = &ctx.kv();
    dev_ = &ctx.device();
    load();
    g_sh = &sh_;
    render(dev_->nowMs());
  }

  void onEvent(const os::Msg& m) override {
    if (m.topic != kTopicLed) return;
    if (m.arg & 1) save();
    render(dev_->nowMs());
  }

  uint32_t onTick(uint32_t now) override {
    render(now);
    // Animated: ~25 fps. Off or static: the wake-up message does the work, the slow poll is
    // only a safety net for a dropped message.
    return (sh_.on.load() && sh_.effect.load() != kSolid) ? 40 : 200;
  }

 private:
  void load() {
    uint32_t col = 0, mode = 0;
    if (kv_->getU32("col", col)) {
      sh_.hue = clampi((int)(col & 0x1FF), 0, 359);
      sh_.sat = clampi((int)((col >> 9) & 0x7F), 0, 100);
      sh_.bright = clampi((int)((col >> 16) & 0xFF), 1, 100);
    }
    if (kv_->getU32("mode", mode)) {
      sh_.on = (int)(mode & 1);
      sh_.effect = clampi((int)((mode >> 1) & 7), 0, kEffectCount - 1);
      sh_.speed = clampi((int)((mode >> 4) & 0x7F), 1, 100);
    }
  }

  void save() {
    kv_->setU32("col", (uint32_t)sh_.hue.load() | ((uint32_t)sh_.sat.load() << 9) |
                           ((uint32_t)sh_.bright.load() << 16));
    kv_->setU32("mode", (uint32_t)sh_.on.load() | ((uint32_t)sh_.effect.load() << 1) |
                            ((uint32_t)sh_.speed.load() << 4));
  }

  // Computes the colour for time `now` and sends it when it changed. Cheap: integer math only.
  void render(uint32_t now) {
    uint32_t rgb = 0;
    if (sh_.on.load()) {
      const int b = clampi(sh_.bright.load(), 1, 100);
      const int speed = clampi(sh_.speed.load(), 1, 100);
      int v = 255 * b * b / 10000;            // quadratic: finer steps at the dim end
      if (v < 1) v = 1;
      int h = sh_.hue.load(), s = sh_.sat.load();
      switch (sh_.effect.load()) {
        case kBreathe: {
          const uint32_t period = 4200 - (uint32_t)speed * 36;
          const uint32_t ph = now % period;
          const uint32_t tri = (ph < period / 2 ? ph : period - ph) * 2000 / period;   // 0..1000
          const uint32_t level = tri * tri / 1000;                                       // eased
          v = v * (int)(30 + level * 970 / 1000) / 1000;
          if (v < 1) v = 1;
          break;
        }
        case kBlink: {
          const uint32_t period = 1600 - (uint32_t)speed * 14;
          if (now % period >= period / 2) v = 0;
          break;
        }
        case kRainbow: {
          const uint32_t period = 12000 - (uint32_t)speed * 110;
          h = (int)((now % period) * 360 / period);
          s = 100;
          break;
        }
        default: break;
      }
      rgb = hsv(h, s, v);
    }
    if (rgb == last_ && sent_) return;
    last_ = rgb;
    sent_ = true;
    sh_.shown.store(rgb);
    if (!led_) return;
    if (rgb == 0) led_->off();
    else led_->set((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
  }

  Shared sh_;
  os::Led* led_ = nullptr;
  os::Kv* kv_ = nullptr;
  os::Device* dev_ = nullptr;
  uint32_t last_ = 0;
  bool sent_ = false;
};

// ---------------------------------------------------------------------------------------------
struct Preset { const char* name; int hue, sat; };
constexpr Preset kPresets[] = {
    {"red", 0, 100},    {"orange", 30, 100}, {"yellow", 52, 100}, {"green", 120, 100}, {"cyan", 180, 100},
    {"blue", 240, 100}, {"purple", 275, 100}, {"pink", 320, 100},  {"white", 0, 0},
};
constexpr int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);

const char* const kEffectMap[] = {"Solid", "Breathe", "Blink", "Rainbow", ""};

constexpr int kColW = 288;     // right column width
constexpr int kColX = 168;     // right column x in root coordinates

class LedApp final : public os::App {
 public:
  void onCreate(os::Context& ctx) override { ctx_ = &ctx; }

  void onStart(lv_obj_t* root) override {
    sh_ = g_sh;
    if (!sh_) {
      lv_obj_t* l = lv_label_create(root);
      lv_label_set_text(l, "LED service not running");
      lv_obj_set_style_text_color(l, lv_color_white(), 0);
      lv_obj_center(l);
      return;
    }
    // ---- left column: power + live preview (x 0..150) ----
    lv_obj_t* pw = label(root, "Power", 0, 6);
    lv_obj_set_style_text_font(pw, &lv_font_montserrat_20, 0);
    sw_ = lv_switch_create(root);
    lv_obj_set_size(sw_, 56, 30);
    lv_obj_set_pos(sw_, 94, 0);
    if (sh_->on.load()) lv_obj_add_state(sw_, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_, onPower, LV_EVENT_VALUE_CHANGED, this);

    swatch_ = lv_obj_create(root);
    lv_obj_remove_style_all(swatch_);
    lv_obj_set_pos(swatch_, 0, 44);
    lv_obj_set_size(swatch_, 150, 184);
    lv_obj_set_style_radius(swatch_, 14, 0);
    lv_obj_set_style_bg_opa(swatch_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(swatch_, 2, 0);
    lv_obj_set_style_border_color(swatch_, lv_color_hex(0x4A5360), 0);
    lv_obj_remove_flag(swatch_, LV_OBJ_FLAG_CLICKABLE);
    refreshSwatch();

    // ---- right column: hue / saturation / brightness / speed ----
    hue_lbl_ = label(root, "", kColX, 0);
    buildHueStrip(root, 22);
    hue_ = slider(root, 22, 0, 359, sh_->hue.load(), true);
    sat_lbl_ = label(root, "", kColX, 44);
    sat_ = slider(root, 66, 0, 100, sh_->sat.load(), false);
    bri_lbl_ = label(root, "", kColX, 88);
    bri_ = slider(root, 110, 1, 100, sh_->bright.load(), false);
    spd_lbl_ = label(root, "", kColX, 132);
    spd_ = slider(root, 154, 1, 100, sh_->speed.load(), false);
    refreshLabels();

    // ---- effects ----
    fx_ = lv_buttonmatrix_create(root);
    lv_buttonmatrix_set_map(fx_, kEffectMap);
    lv_buttonmatrix_set_button_ctrl_all(fx_, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(fx_, true);
    lv_buttonmatrix_set_button_ctrl(fx_, (uint32_t)sh_->effect.load(), LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_pos(fx_, kColX, 180);
    lv_obj_set_size(fx_, kColW, 44);
    lv_obj_set_style_bg_opa(fx_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(fx_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(fx_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(fx_, 6, LV_PART_MAIN);
    lv_obj_set_style_text_font(fx_, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_text_color(fx_, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(fx_, lv_color_hex(0x2A323D), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(fx_, lv_color_hex(0x2E7BD6), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(fx_, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(fx_, 10, LV_PART_ITEMS);
    lv_obj_add_event_cb(fx_, onEffect, LV_EVENT_VALUE_CHANGED, this);

    // ---- preset swatches: 9 x 44 px across the full 456 px, y 234..264 ----
    for (int i = 0; i < kNumPresets; i++) {
      lv_obj_t* b = lv_button_create(root);
      lv_obj_set_size(b, 44, 30);
      lv_obj_set_pos(b, i * (456 - 44) / (kNumPresets - 1), 234);
      lv_obj_set_style_radius(b, 8, 0);
      lv_obj_set_style_shadow_width(b, 0, 0);
      lv_obj_set_style_border_width(b, 1, 0);
      lv_obj_set_style_border_color(b, lv_color_hex(0x4A5360), 0);
      const uint32_t c = hsv(kPresets[i].hue, kPresets[i].sat, 255);
      lv_obj_set_style_bg_color(b, lv_color_hex(c), 0);
      lv_obj_add_event_cb(b, onPreset, LV_EVENT_CLICKED, this);
      lv_obj_set_user_data(b, reinterpret_cast<void*>((intptr_t)i));
    }

    timer_ = lv_timer_create(onTimer, 100, this);
  }

  void onStop() override {
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
  }

 private:
  static lv_obj_t* label(lv_obj_t* root, const char* text, int x, int y) {
    lv_obj_t* l = lv_label_create(root);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC8D0DA), 0);
    lv_obj_set_pos(l, x, y);
    return l;
  }

  // Slider 288 x 14 in the right column at row y.
  lv_obj_t* slider(lv_obj_t* root, int y, int lo, int hi, int val, bool transparent_track) {
    lv_obj_t* s = lv_slider_create(root);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_pos(s, kColX, y);
    lv_obj_set_size(s, kColW, 14);
    lv_obj_set_ext_click_area(s, 12);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2A323D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2E7BD6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 3, LV_PART_KNOB);
    if (transparent_track) {          // lets the rainbow strip behind show through
      lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_INDICATOR);
    }
    lv_obj_add_event_cb(s, onSlider, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(s, onRelease, LV_EVENT_RELEASED, this);
    return s;
  }

  // Six 48 px two-stop gradients (the LVGL build allows only 2 stops per gradient).
  static void buildHueStrip(lv_obj_t* root, int y) {
    for (int i = 0; i < 6; i++) {
      lv_obj_t* seg = lv_obj_create(root);
      lv_obj_remove_style_all(seg);
      lv_obj_set_pos(seg, kColX + i * 48, y);
      lv_obj_set_size(seg, 48, 14);
      lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(seg, lv_color_hex(hsv(i * 60, 100, 255)), 0);
      lv_obj_set_style_bg_grad_color(seg, lv_color_hex(hsv(i * 60 + 60, 100, 255)), 0);
      lv_obj_set_style_bg_grad_dir(seg, LV_GRAD_DIR_HOR, 0);
      lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    }
  }

  void refreshLabels() {
    lv_label_set_text_fmt(hue_lbl_, "Hue  %d", sh_->hue.load());
    lv_label_set_text_fmt(sat_lbl_, "Saturation  %d%%", sh_->sat.load());
    lv_label_set_text_fmt(bri_lbl_, "Brightness  %d%%", sh_->bright.load());
    lv_label_set_text_fmt(spd_lbl_, "Speed  %d%%", sh_->speed.load());
  }

  void refreshSwatch() {
    const uint32_t c = sh_->shown.load();
    lv_obj_set_style_bg_color(swatch_, lv_color_hex(sh_->on.load() ? c : 0x151A20), 0);
  }

  void notify(bool commit) {
    os::Msg m{kTopicLed, 0, 0, commit ? 1u : 0u, ctx_->device().nowMs()};
    ctx_->bus().publish(m);
  }

  static void onTimer(lv_timer_t* t) {
    auto* self = static_cast<LedApp*>(lv_timer_get_user_data(t));
    self->refreshSwatch();
  }

  static void onPower(lv_event_t* e) {
    auto* self = static_cast<LedApp*>(lv_event_get_user_data(e));
    self->sh_->on = lv_obj_has_state(self->sw_, LV_STATE_CHECKED) ? 1 : 0;
    self->notify(true);
  }

  static void onSlider(lv_event_t* e) {
    auto* self = static_cast<LedApp*>(lv_event_get_user_data(e));
    lv_obj_t* s = lv_event_get_target_obj(e);
    const int v = lv_slider_get_value(s);
    if (s == self->hue_) self->sh_->hue = v;
    else if (s == self->sat_) self->sh_->sat = v;
    else if (s == self->bri_) self->sh_->bright = v;
    else if (s == self->spd_) self->sh_->speed = v;
    self->refreshLabels();
    const uint32_t now = self->ctx_->device().nowMs();
    if (now - self->last_live_ >= 40) {       // do not flood the bus while dragging
      self->last_live_ = now;
      self->notify(false);
    }
  }

  static void onRelease(lv_event_t* e) {
    static_cast<LedApp*>(lv_event_get_user_data(e))->notify(true);
  }

  static void onEffect(lv_event_t* e) {
    auto* self = static_cast<LedApp*>(lv_event_get_user_data(e));
    const uint32_t idx = lv_buttonmatrix_get_selected_button(self->fx_);
    if (idx < (uint32_t)kEffectCount) self->sh_->effect = (int)idx;
    self->notify(true);
  }

  static void onPreset(lv_event_t* e) {
    auto* self = static_cast<LedApp*>(lv_event_get_user_data(e));
    const int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (i < 0 || i >= kNumPresets) return;
    self->sh_->hue = kPresets[i].hue;
    self->sh_->sat = kPresets[i].sat;
    lv_slider_set_value(self->hue_, kPresets[i].hue, LV_ANIM_OFF);
    lv_slider_set_value(self->sat_, kPresets[i].sat, LV_ANIM_OFF);
    self->refreshLabels();
    self->notify(true);
  }

  os::Context* ctx_ = nullptr;
  Shared* sh_ = nullptr;
  lv_obj_t *sw_ = nullptr, *swatch_ = nullptr, *fx_ = nullptr;
  lv_obj_t *hue_ = nullptr, *sat_ = nullptr, *bri_ = nullptr, *spd_ = nullptr;
  lv_obj_t *hue_lbl_ = nullptr, *sat_lbl_ = nullptr, *bri_lbl_ = nullptr, *spd_lbl_ = nullptr;
  lv_timer_t* timer_ = nullptr;
  uint32_t last_live_ = 0;
};

}  // namespace

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "led",
    .name = "LED",
    .icon = nullptr,
    .glyph = LV_SYMBOL_EYE_OPEN,
    .perms = os::LED | os::NVS,
    .psram_arena = 16384,
    .sram_budget = 0,
    .background = true,
    .make = os::makeApp<LedApp>,
    .make_service = os::makeService<LedService>,
};

}  // namespace apps::led
