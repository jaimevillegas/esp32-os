#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <vector>

#include "os/app.h"

namespace apps::diag {

// Background part: logs a heap/CPU summary every 30 s, proving the `svc` task and an arena that
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
    os::logf("diag.svc", "heap internal %u free / %u min, PSRAM %u free, load %d/%d%%, temp %d (0.1C)",
             (unsigned)si.int_free, (unsigned)si.int_min, (unsigned)si.psram_free, si.cpu_load[0],
             si.cpu_load[1], si.temp_c10);
    return 30000;
  }

 private:
  os::Context* ctx_ = nullptr;
};

namespace {

// Palette (matches the rest of the UI).
constexpr uint32_t kBg = 0x0E1116, kCard = 0x1C2128, kTrack = 0x2A313A;
constexpr uint32_t kText = 0xE6EDF3, kDim = 0x8B949E;
constexpr uint32_t kGreen = 0x3FB950, kAmber = 0xD29922, kRed = 0xF85149, kBlue = 0x58A6FF;

// Layout, in pixels, inside the 456x264 content area of the app root.
constexpr int kGap = 6;
constexpr int kChipH = 46;
constexpr int kLeftW = 282, kRightW = 456 - kLeftW - kGap;           // 282 / 168
constexpr int kBodyY = kChipH + kGap;                                // 52
constexpr int kMemH = 146, kChartH = 60;                             // 146 + 6 + 60 = 212
constexpr int kBatH = 68, kCpuH = 84, kStackH = 48;                  // 68 + 6 + 84 + 6 + 48 = 212
constexpr int kPad = 8, kBarH = 10, kRowH = 34;
constexpr int kHistory = 60;
constexpr int kBarMax = 1000;                                        // bar range (permille)

struct Text {                       // label whose text lives in our own buffer: no allocation on update
  lv_obj_t* obj = nullptr;
  char buf[40] = "";
  uint32_t color = 0;
  void set(const char* s) {
    if (!strcmp(buf, s)) return;
    snprintf(buf, sizeof buf, "%s", s);
    lv_label_set_text_static(obj, buf);
  }
  void setColor(uint32_t c) {
    if (c == color) return;
    color = c;
    lv_obj_set_style_text_color(obj, lv_color_hex(c), 0);
  }
};

struct Row {                        // "left text ... right text" + bar (+ optional marker)
  Text l, r;
  lv_obj_t* bar = nullptr;
  lv_obj_t* mark = nullptr;
  int bar_x = 0, bar_w = 0;
  int last_mark = -2;
  uint32_t bar_color = 0;
  int last_val = -1;
};

lv_obj_t* makeCard(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(kCard), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 8, 0);
  lv_obj_set_scrollable(c, false);
  lv_obj_set_clickable(c, false);
  return c;
}

void initText(Text& t, lv_obj_t* parent, const lv_font_t* font, uint32_t color, int x, int y, int w,
              lv_text_align_t align) {
  t.obj = lv_label_create(parent);
  lv_obj_set_pos(t.obj, x, y);
  lv_obj_set_width(t.obj, w);
  lv_label_set_long_mode(t.obj, LV_LABEL_LONG_MODE_CLIP);
  lv_obj_set_style_text_font(t.obj, font, 0);
  lv_obj_set_style_text_align(t.obj, align, 0);
  t.color = color;
  lv_obj_set_style_text_color(t.obj, lv_color_hex(color), 0);
  lv_label_set_text_static(t.obj, t.buf);
}

// Row of height 28: text line (16) at y, bar (10) at y + 18.
void initRow(Row& r, lv_obj_t* card, int x, int y, int w, int lw, bool marker) {
  initText(r.l, card, &lv_font_montserrat_14, kText, x, y, lw, LV_TEXT_ALIGN_LEFT);
  initText(r.r, card, &lv_font_montserrat_14, kDim, x + lw, y, w - lw, LV_TEXT_ALIGN_RIGHT);
  r.bar = lv_bar_create(card);
  lv_bar_set_range(r.bar, 0, kBarMax);
  lv_obj_set_pos(r.bar, x, y + 18);
  lv_obj_set_size(r.bar, w, kBarH);
  lv_obj_set_style_radius(r.bar, kBarH / 2, 0);
  lv_obj_set_style_radius(r.bar, kBarH / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(r.bar, lv_color_hex(kTrack), 0);
  lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, LV_PART_INDICATOR);
  r.bar_x = x;
  r.bar_w = w;
  if (marker) {
    r.mark = lv_obj_create(card);
    lv_obj_remove_style_all(r.mark);
    lv_obj_set_size(r.mark, 2, kBarH + 6);
    lv_obj_set_y(r.mark, y + 18 - 3);
    lv_obj_set_style_bg_color(r.mark, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(r.mark, LV_OPA_COVER, 0);
    lv_obj_set_clickable(r.mark, false);
    lv_obj_set_hidden(r.mark, true);
  }
}

// permille < 0 means "unavailable". mark < 0 hides the marker.
void setRow(Row& r, const char* left, const char* right, int permille, uint32_t color, int mark = -1) {
  r.l.set(left);
  r.r.set(right);
  const int v = permille < 0 ? 0 : permille > kBarMax ? kBarMax : permille;
  if (v != r.last_val) {
    r.last_val = v;
    lv_bar_set_value(r.bar, v, LV_ANIM_OFF);
  }
  if (color != r.bar_color) {
    r.bar_color = color;
    lv_obj_set_style_bg_color(r.bar, lv_color_hex(color), LV_PART_INDICATOR);
  }
  if (r.mark && mark != r.last_mark) {
    r.last_mark = mark;
    if (mark < 0) {
      lv_obj_set_hidden(r.mark, true);
    } else {
      lv_obj_set_hidden(r.mark, false);
      lv_obj_set_x(r.mark, r.bar_x + (int)((int64_t)r.bar_w * mark / kBarMax) - 1);
    }
  }
}

uint32_t usedColor(int used_permille) {            // fuller = worse
  return used_permille < 700 ? kGreen : used_permille < 880 ? kAmber : kRed;
}
uint32_t loadColor(int pct) { return pct < 60 ? kGreen : pct < 85 ? kAmber : kRed; }

void fmtSize(char* out, size_t n, uint32_t b) {   // "182K" / "7.9M"
  if (b >= 1024u * 1024u) {
    const uint32_t t = (uint32_t)(((uint64_t)b * 10 + 524288) / 1048576);
    snprintf(out, n, "%u.%uM", (unsigned)(t / 10), (unsigned)(t % 10));
  } else {
    snprintf(out, n, "%uK", (unsigned)((b + 512) / 1024));
  }
}

int permille(uint32_t part, uint32_t whole) {
  return whole ? (int)((uint64_t)part * kBarMax / whole) : -1;
}

}  // namespace

class DiagApp final : public os::App {
 public:
  void onCreate(os::Context& ctx) override {
    ctx_ = &ctx;
    // Sparkline samples live in the app arena (freed with it); the chart reads them in place.
    void* p = ctx.mem().allocate(sizeof(std::pmr::vector<int32_t>), alignof(std::pmr::vector<int32_t>));
    hist_ = new (p) std::pmr::vector<int32_t>(kHistory, (int32_t)LV_CHART_POINT_NONE, &ctx.mem());
  }

  void onStart(lv_obj_t* root) override {
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, lv_color_hex(kBg), 0);

    // ---- header chips: 118 + 112 + 108 + 106 + 3 * 4 = 456 ----
    static const char* const kCap[4] = {"UPTIME", "CPU", "CHIP TEMP", "TASKS"};
    static const int kChipW[4] = {118, 112, 108, 106};
    int x = 0;
    for (int i = 0; i < 4; i++) {
      lv_obj_t* c = makeCard(root, x, 0, kChipW[i], kChipH);
      initText(chipCap_[i], c, &lv_font_montserrat_14, kDim, 8, 3, kChipW[i] - 16, LV_TEXT_ALIGN_LEFT);
      initText(chipVal_[i], c, &lv_font_montserrat_20, kText, 8, 19, kChipW[i] - 16, LV_TEXT_ALIGN_LEFT);
      chipCap_[i].set(kCap[i]);
      x += kChipW[i] + 4;
    }

    // ---- memory card: 282 x 146, four rows of 28 px every 34 px, inner width 266 ----
    lv_obj_t* mem = makeCard(root, 0, kBodyY, kLeftW, kMemH);
    const int iw = kLeftW - 2 * kPad;
    for (int i = 0; i < 4; i++) initRow(mem_[i], mem, kPad, kPad + i * kRowH, iw, 112, i < 2);

    // ---- sparkline card: 282 x 60 ----
    lv_obj_t* sp = makeCard(root, 0, kBodyY + kMemH + kGap, kLeftW, kChartH);
    initText(spCap_, sp, &lv_font_montserrat_14, kDim, kPad, 6, 72, LV_TEXT_ALIGN_LEFT);
    initText(spHi_, sp, &lv_font_montserrat_14, kText, kPad, 22, 72, LV_TEXT_ALIGN_LEFT);
    initText(spLo_, sp, &lv_font_montserrat_14, kText, kPad, 38, 72, LV_TEXT_ALIGN_LEFT);
    spCap_.set("SRAM 60 s");
    chart_ = lv_chart_create(sp);
    lv_obj_set_pos(chart_, 84, 6);
    lv_obj_set_size(chart_, kLeftW - 84 - kPad, kChartH - 12);        // 190 x 48
    lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_, kHistory);
    lv_chart_set_div_line_count(chart_, 0, 0);
    lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(chart_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart_, 0, 0);
    lv_obj_set_style_pad_all(chart_, 2, 0);
    lv_obj_set_style_line_width(chart_, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart_, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_clickable(chart_, false);
    series_ = lv_chart_add_series(chart_, lv_color_hex(kBlue), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_y_array(chart_, series_, hist_->data());

    // ---- right column, 168 wide, inner width 152 ----
    const int rx = kLeftW + kGap, rw = kRightW - 2 * kPad;
    lv_obj_t* bat = makeCard(root, rx, kBodyY, kRightW, kBatH);
    initRow(bat_, bat, kPad, kPad, rw, 64, false);
    initText(batState_, bat, &lv_font_montserrat_14, kDim, kPad, kPad + 34, rw, LV_TEXT_ALIGN_LEFT);

    lv_obj_t* cpu = makeCard(root, rx, kBodyY + kBatH + kGap, kRightW, kCpuH);
    for (int i = 0; i < 2; i++) initRow(cpuRow_[i], cpu, kPad, 10 + i * kRowH, rw, 76, false);

    lv_obj_t* stk = makeCard(root, rx, kBodyY + kBatH + kCpuH + 2 * kGap, kRightW, kStackH);
    initRow(stack_, stk, kPad, 10, rw, 84, false);

    refresh();
    timer_ = lv_timer_create(onTimer, 1000, this);
  }

  void onStop() override {
    if (timer_) lv_timer_delete(timer_);     // timers are not children of the root
    timer_ = nullptr;
  }

  void onDestroy() override { hist_->~vector(); }

 private:
  static void onTimer(lv_timer_t* t) { static_cast<DiagApp*>(lv_timer_get_user_data(t))->refresh(); }

  void refresh() {
    os::SysInfo si;
    ctx_->device().sysInfo(si);
    const os::PowerInfo pw = ctx_->device().power();
    char a[40], b[24], c[24], r[40];

    // header chips
    if (si.uptime_s >= 86400)
      snprintf(a, sizeof a, "%ud %02uh", (unsigned)(si.uptime_s / 86400), (unsigned)(si.uptime_s / 3600 % 24));
    else
      snprintf(a, sizeof a, "%u:%02u:%02u", (unsigned)(si.uptime_s / 3600), (unsigned)(si.uptime_s / 60 % 60),
               (unsigned)(si.uptime_s % 60));
    chipVal_[0].set(a);
    if (si.cpu_mhz) snprintf(a, sizeof a, "%u MHz", si.cpu_mhz);
    else snprintf(a, sizeof a, "--");
    chipVal_[1].set(a);
    if (si.temp_c10 != INT16_MIN) {
      const int t = si.temp_c10;
      snprintf(a, sizeof a, "%d.%d\xC2\xB0" "C", t / 10, (t < 0 ? -t : t) % 10);
      chipVal_[2].setColor(t < 600 ? kText : t < 750 ? kAmber : kRed);
    } else {
      snprintf(a, sizeof a, "--");
    }
    chipVal_[2].set(a);
    snprintf(a, sizeof a, "%u", si.n_tasks);
    chipVal_[3].set(a);

    // internal SRAM: bar = used, marker = lowest free ever seen
    fmtSize(b, sizeof b, si.int_largest);
    snprintf(a, sizeof a, "SRAM  blk %s", b);
    fmtSize(b, sizeof b, si.int_free);
    fmtSize(c, sizeof c, si.int_min);
    snprintf(r, sizeof r, "%s free  min %s", b, c);
    int used = permille(si.int_total - si.int_free, si.int_total);
    setRow(mem_[0], a, r, used, used < 0 ? kGreen : usedColor(used),
           permille(si.int_total - si.int_min, si.int_total));

    fmtSize(b, sizeof b, si.psram_largest);
    snprintf(a, sizeof a, "PSRAM  blk %s", b);
    fmtSize(b, sizeof b, si.psram_free);
    fmtSize(c, sizeof c, si.psram_min);
    snprintf(r, sizeof r, "%s free  min %s", b, c);
    used = permille(si.psram_total - si.psram_free, si.psram_total);
    setRow(mem_[1], a, r, used, used < 0 ? kGreen : usedColor(used),
           permille(si.psram_total - si.psram_min, si.psram_total));

    fmtSize(b, sizeof b, si.dma_free);
    fmtSize(c, sizeof c, si.dma_largest);
    snprintf(r, sizeof r, "%s free  blk %s", b, c);
    used = permille(si.dma_total - si.dma_free, si.dma_total);
    setRow(mem_[2], "DMA memory", r, used, used < 0 ? kGreen : usedColor(used));

    if (si.app_size && si.app_slot_size) {
      fmtSize(b, sizeof b, si.app_size);
      fmtSize(c, sizeof c, si.app_slot_size);
      snprintf(r, sizeof r, "%s of %s", b, c);
      used = permille(si.app_size, si.app_slot_size);
      if (used > 0 && used < 5) used = 5;               // keep a sliver visible
      setRow(mem_[3], "Flash app slot", r, used, usedColor(used));
    } else {
      setRow(mem_[3], "Flash app slot", "--", -1, kGreen);
    }

    // sparkline of free internal SRAM (KB), samples are stored in the arena
    lv_chart_set_next_value(chart_, series_, (int32_t)(si.int_free / 1024));
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (int32_t v : *hist_) {
      if (v == LV_CHART_POINT_NONE) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (hi - lo < 8) {
      const int32_t m = (hi + lo) / 2;
      lo = m - 4;
      hi = m + 4;
    }
    if (lo != chartLo_ || hi != chartHi_) {
      chartLo_ = lo;
      chartHi_ = hi;
      lv_chart_set_axis_range(chart_, LV_CHART_AXIS_PRIMARY_Y, lo - 1, hi + 1);
    }
    snprintf(a, sizeof a, "hi %dK", (int)hi);
    spHi_.set(a);
    snprintf(a, sizeof a, "lo %dK", (int)lo);
    spLo_.set(a);

    // battery
    if (pw.vbat_mv) {
      snprintf(r, sizeof r, "%u.%02u V  %u%%", pw.vbat_mv / 1000, pw.vbat_mv % 1000 / 10, pw.percent);
      setRow(bat_, "Battery", r, pw.percent * 10,
             pw.charging ? kBlue : pw.percent < 15 ? kRed : pw.percent < 35 ? kAmber : kGreen);
    } else {
      setRow(bat_, "Battery", "--", -1, kGreen);
    }
    snprintf(a, sizeof a, "%s%s", pw.usb ? "USB" : "On battery", pw.charging ? " + charging" : "");
    batState_.set(a);

    // CPU load per core
    for (int i = 0; i < 2; i++) {
      snprintf(a, sizeof a, "Core %d", i);
      const int ld = si.cpu_load[i];
      if (ld >= 0) {
        snprintf(r, sizeof r, "%d%%", ld);
        setRow(cpuRow_[i], a, r, ld * 10, loadColor(ld));
      } else {
        setRow(cpuRow_[i], a, "--", -1, kGreen);
      }
    }

    // UI task stack margin: the bar scale is 0..8 KB of headroom
    snprintf(r, sizeof r, "%u B", (unsigned)si.ui_stack_free);
    const uint32_t sf = si.ui_stack_free > 8192 ? 8192 : si.ui_stack_free;
    setRow(stack_, "UI stack free", r, (int)((uint64_t)sf * kBarMax / 8192),
           si.ui_stack_free < 512 ? kRed : si.ui_stack_free < 1024 ? kAmber : kGreen);
  }

  os::Context* ctx_ = nullptr;
  lv_timer_t* timer_ = nullptr;
  std::pmr::vector<int32_t>* hist_ = nullptr;
  lv_obj_t* chart_ = nullptr;
  lv_chart_series_t* series_ = nullptr;
  int32_t chartLo_ = INT32_MIN, chartHi_ = INT32_MIN;
  Text chipCap_[4], chipVal_[4];
  Text spCap_, spHi_, spLo_, batState_;
  Row mem_[4], bat_, cpuRow_[2], stack_;
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
