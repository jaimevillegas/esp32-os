#include <cstdio>
#include <memory_resource>
#include <vector>

#include "os/app.h"

namespace apps::diag {

// Background part: logs a heap summary every 30 s, proving the `svc` task and an arena that
// outlives the UI.
class DiagService final : public os::Service {
 public:
  void onCreate(os::Context& ctx) override { ctx_ = &ctx; }
  void onEvent(const os::Msg& m) override {
    if (m.topic == os::Topic::Power) {
      os::logf("diag.svc", "power event: %u%% usb=%d charging=%d", (unsigned)(m.arg & 0xFF),
               !!(m.arg & 0x100), !!(m.arg & 0x200));
    }
  }
  uint32_t onTick(uint32_t) override {
    os::SysInfo si;
    ctx_->device().sysInfo(si);
    os::logf("diag.svc", "heap internal %u free / %u min, PSRAM %u free", (unsigned)si.int_free,
             (unsigned)si.int_min, (unsigned)si.psram_free);
    return 30000;
  }

 private:
  os::Context* ctx_ = nullptr;
};

class DiagApp final : public os::App {
 public:
  void onCreate(os::Context& ctx) override {
    ctx_ = &ctx;
    // A container that lives in the app arena: freed with it, never leaked.
    void* p = ctx.mem().allocate(sizeof(std::pmr::vector<uint32_t>), alignof(std::pmr::vector<uint32_t>));
    hist_ = new (p) std::pmr::vector<uint32_t>(&ctx.mem());
    hist_->reserve(kHistory);
  }

  void onStart(lv_obj_t* root) override {
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 4, 0);
    for (auto& l : lines_) {
      l = lv_label_create(root);
      lv_obj_set_style_text_color(l, lv_color_white(), 0);
      lv_obj_set_width(l, lv_pct(100));
    }
    refresh();
    timer_ = lv_timer_create(onTimer, 1000, this);
  }

  void onStop() override {
    if (timer_) lv_timer_delete(timer_);     // timers are not children of the root
    timer_ = nullptr;
  }

  void onDestroy() override { hist_->~vector(); }

 private:
  static constexpr size_t kHistory = 30;
  static void onTimer(lv_timer_t* t) { static_cast<DiagApp*>(lv_timer_get_user_data(t))->refresh(); }

  void refresh() {
    os::SysInfo si;
    ctx_->device().sysInfo(si);
    const os::PowerInfo pw = ctx_->device().power();
    if (hist_->size() == kHistory) hist_->erase(hist_->begin());
    hist_->push_back(si.int_free);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t v : *hist_) { if (v < lo) lo = v; if (v > hi) hi = v; }

    lv_label_set_text_fmt(lines_[0], "Internal SRAM   free %u KB   min %u KB   block %u KB",
                          (unsigned)(si.int_free / 1024), (unsigned)(si.int_min / 1024),
                          (unsigned)(si.int_largest / 1024));
    lv_label_set_text_fmt(lines_[1], "PSRAM          free %u KB   min %u KB",
                          (unsigned)(si.psram_free / 1024), (unsigned)(si.psram_min / 1024));
    lv_label_set_text_fmt(lines_[2], "Free SRAM, last %us: %u..%u KB", (unsigned)hist_->size(),
                          (unsigned)(lo / 1024), (unsigned)(hi / 1024));
    lv_label_set_text_fmt(lines_[3], "UI task stack free: %u B", (unsigned)si.ui_stack_free);
    lv_label_set_text_fmt(lines_[4], "Battery %u mV   %u%%   usb %s   charging %s", pw.vbat_mv, pw.percent,
                          pw.usb ? "yes" : "no", pw.charging ? "yes" : "no");
    lv_label_set_text_fmt(lines_[5], "Uptime %u:%02u:%02u", (unsigned)(si.uptime_s / 3600),
                          (unsigned)(si.uptime_s / 60 % 60), (unsigned)(si.uptime_s % 60));
  }

  os::Context* ctx_ = nullptr;
  lv_obj_t* lines_[6] = {};
  lv_timer_t* timer_ = nullptr;
  std::pmr::vector<uint32_t>* hist_ = nullptr;
};

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "diag",
    .name = "Diagnostics",
    .icon = nullptr,
    .glyph = LV_SYMBOL_LIST,
    .perms = 0,
    .psram_arena = 64 * 1024,
    .sram_budget = 0,
    .background = true,
    .make = os::makeApp<DiagApp>,
    .make_service = os::makeService<DiagService>,
};

}  // namespace apps::diag
