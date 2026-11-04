// Music player UI. The playlist and the auto-advance logic live in MusicService (a background
// service); this class only displays the state and forwards user actions.
#include <cstdio>
#include <cstring>

#include "music_svc.h"
#include "os/app.h"

namespace apps::music {

namespace {

constexpr int kW = 456;              // usable width inside the 12 px padding
constexpr int kRowH = 30;
constexpr int kListY = 142;
constexpr int kListH = 264 - kListY;
constexpr int kRows = kListH / kRowH + 2;      // visible rows plus one on each side
constexpr uint32_t kRefreshMs = 200;           // 5 Hz while visible

void fmtTime(char* out, size_t cap, uint32_t ms) {
  const uint32_t s = ms / 1000;
  snprintf(out, cap, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

}  // namespace

class MusicApp final : public os::App {
 public:
  void onCreate(os::Context& ctx) override {
    ctx_ = &ctx;
    svc_ = service();
  }

  void onStart(lv_obj_t* root) override {
    audio_ = ctx_->audio();
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);

    // ---- title and track counter ----
    title_ = lv_label_create(root);
    lv_obj_set_width(title_, 370);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_, lv_color_white(), 0);
    lv_obj_set_pos(title_, 0, 0);
    lv_label_set_text(title_, "");

    index_ = lv_label_create(root);
    lv_obj_set_width(index_, 80);
    lv_obj_set_style_text_align(index_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(index_, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_pos(index_, kW - 80, 4);
    lv_label_set_text(index_, "");

    // ---- progress ----
    elapsed_ = timeLabel(root, 0, LV_TEXT_ALIGN_LEFT);
    total_ = timeLabel(root, kW - 52, LV_TEXT_ALIGN_RIGHT);
    seek_ = lv_slider_create(root);
    lv_slider_set_range(seek_, 0, 1000);
    lv_obj_set_size(seek_, kW - 116, 8);
    lv_obj_set_pos(seek_, 58, 38);
    lv_obj_set_ext_click_area(seek_, 14);
    lv_obj_add_event_cb(seek_, onSeek, LV_EVENT_ALL, this);

    // ---- transport buttons ----
    lv_obj_t* bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kW, 42);
    lv_obj_set_pos(bar, 0, 54);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(bar, false);
    btn_shuffle_ = button(bar, LV_SYMBOL_SHUFFLE, 56, onShuffle, &lbl_shuffle_);
    btn_prev_ = button(bar, LV_SYMBOL_PREV, 70, onPrev, nullptr);
    btn_play_ = button(bar, LV_SYMBOL_PLAY, 90, onPlay, &lbl_play_);
    btn_stop_ = button(bar, LV_SYMBOL_STOP, 70, onStop, nullptr);
    btn_next_ = button(bar, LV_SYMBOL_NEXT, 70, onNext, nullptr);
    btn_repeat_ = button(bar, LV_SYMBOL_LOOP, 56, onRepeat, &lbl_repeat_);

    // ---- volume ----
    lv_obj_t* vi = lv_label_create(root);
    lv_label_set_text(vi, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vi, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_pos(vi, 0, 108);
    vol_ = lv_slider_create(root);
    lv_slider_set_range(vol_, 0, 100);
    lv_obj_set_size(vol_, kW - 116, 8);
    lv_obj_set_pos(vol_, 40, 113);
    lv_obj_set_ext_click_area(vol_, 14);
    lv_obj_add_event_cb(vol_, onVolume, LV_EVENT_ALL, this);
    vol_pct_ = timeLabel(root, kW - 52, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_y(vol_pct_, 106);

    // ---- virtual track list: a fixed pool of row widgets rebound while scrolling ----
    list_ = lv_obj_create(root);
    lv_obj_remove_style_all(list_);
    lv_obj_set_size(list_, kW, kListH);
    lv_obj_set_pos(list_, 0, kListY);
    lv_obj_set_style_bg_color(list_, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list_, 6, 0);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_add_event_cb(list_, onListScroll, LV_EVENT_SCROLL, this);
    spacer_ = lv_obj_create(list_);
    lv_obj_remove_style_all(spacer_);
    lv_obj_set_size(spacer_, 1, 1);
    lv_obj_set_clickable(spacer_, false);
    for (int i = 0; i < kRows; i++) {
      Row& r = rows_[i];
      r.owner = this;
      r.idx = -1;
      r.btn = lv_button_create(list_);
      lv_obj_set_size(r.btn, kW - 8, kRowH - 2);
      lv_obj_set_style_radius(r.btn, 4, 0);
      lv_obj_set_style_shadow_width(r.btn, 0, 0);
      lv_obj_set_style_bg_opa(r.btn, LV_OPA_TRANSP, 0);
      lv_obj_set_hidden(r.btn, true);
      lv_obj_add_event_cb(r.btn, onRow, LV_EVENT_CLICKED, &r);
      r.lbl = lv_label_create(r.btn);
      lv_obj_set_width(r.lbl, kW - 24);
      lv_label_set_long_mode(r.lbl, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_style_text_color(r.lbl, lv_color_white(), 0);
      lv_obj_align(r.lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
    msg_ = lv_label_create(root);
    lv_obj_set_width(msg_, kW);
    lv_obj_set_style_text_align(msg_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(msg_, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_pos(msg_, 0, kListY + kListH / 2 - 12);

    forceRefresh();
  }

  void onResume() override {
    if (!timer_) timer_ = lv_timer_create(onTimer, kRefreshMs, this);   // only while visible
    forceRefresh();
  }

  void onPause() override { stopTimer(); }
  void onStop() override { stopTimer(); }

  void onEvent(const os::Msg& m) override {
    if (m.topic == os::Topic::Audio || m.topic == os::Topic::Storage) refresh();
  }

 private:
  struct Row {
    MusicApp* owner;
    lv_obj_t* btn;
    lv_obj_t* lbl;
    int idx;
    bool hl;
  };

  // ---- widget helpers ----
  lv_obj_t* timeLabel(lv_obj_t* root, int x, lv_text_align_t align) {
    lv_obj_t* l = lv_label_create(root);
    lv_obj_set_width(l, 52);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_style_text_color(l, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_pos(l, x, 32);
    lv_label_set_text(l, "0:00");
    return l;
  }

  lv_obj_t* button(lv_obj_t* parent, const char* glyph, int w, lv_event_cb_t cb, lv_obj_t** label_out) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, 40);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, glyph);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
  }

  void stopTimer() {
    if (timer_) lv_timer_delete(timer_);     // timers are not children of the root
    timer_ = nullptr;
  }

  static void setEnabled(lv_obj_t* o, bool en) {
    if (en) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
  }

  static void setActive(lv_obj_t* b, bool on) {
    lv_obj_set_style_bg_color(b, on ? lv_palette_main(LV_PALETTE_BLUE) : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  }

  // ---- state -> widgets ----
  void forceRefresh() {
    version_ = ~0u;
    last_title_[0] = 0;
    refresh();
  }

  void refresh() {
    if (!svc_ || !list_) return;
    const Snapshot sn = svc_->snapshot();
    os::AudioStatus st{};
    if (audio_) audio_->status(st);

    // Title line.
    char title[80];
    if (!sn.sd) snprintf(title, sizeof title, "No microSD card");
    else if (sn.scanning) snprintf(title, sizeof title, "Scanning card...");
    else if (st.state == os::PlayState::Error) snprintf(title, sizeof title, LV_SYMBOL_WARNING " Cannot play: %s", st.title);
    else if (st.state == os::PlayState::Playing || st.state == os::PlayState::Paused) snprintf(title, sizeof title, "%s", st.title);
    else if (sn.count == 0) snprintf(title, sizeof title, "No tracks found");
    else snprintf(title, sizeof title, "Select a track");
    if (strcmp(title, last_title_) != 0) {
      lv_label_set_text(title_, title);
      snprintf(last_title_, sizeof last_title_, "%s", title);
    }

    // Playlist position.
    char idx[16] = "";
    if (sn.current >= 0 && sn.count) snprintf(idx, sizeof idx, "%d / %u", sn.current + 1, (unsigned)sn.count);
    else if (sn.count) snprintf(idx, sizeof idx, "%u tracks", (unsigned)sn.count);
    if (strcmp(idx, last_idx_) != 0) {
      lv_label_set_text(index_, idx);
      snprintf(last_idx_, sizeof last_idx_, "%s", idx);
    }

    // Progress and time.
    const bool active = st.state == os::PlayState::Playing || st.state == os::PlayState::Paused;
    if (!seeking_) {
      char t[16];
      fmtTime(t, sizeof t, active ? st.pos_ms : 0);
      lv_label_set_text(elapsed_, t);
      const int v = (active && st.dur_ms) ? (int)((uint64_t)st.pos_ms * 1000 / st.dur_ms) : 0;
      if (lv_slider_get_value(seek_) != v) lv_slider_set_value(seek_, v, LV_ANIM_OFF);
    }
    if (active && st.dur_ms) {
      char t[16];
      fmtTime(t, sizeof t, st.dur_ms);
      lv_label_set_text(total_, t);
    } else {
      lv_label_set_text(total_, active ? "--:--" : "0:00");
    }
    dur_ms_ = active ? st.dur_ms : 0;

    // Buttons.
    lv_label_set_text(lbl_play_, st.state == os::PlayState::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    const bool usable = sn.sd && !sn.scanning && sn.count > 0 && audio_;
    setEnabled(btn_play_, usable);
    setEnabled(btn_prev_, usable);
    setEnabled(btn_next_, usable);
    setEnabled(btn_stop_, usable);
    setEnabled(btn_shuffle_, usable);
    setEnabled(btn_repeat_, usable);
    setEnabled(seek_, usable && active && st.dur_ms > 0);
    setActive(btn_shuffle_, sn.shuffle);
    setActive(btn_repeat_, sn.repeat != Repeat::Off);
    lv_label_set_text(lbl_repeat_, sn.repeat == Repeat::One ? LV_SYMBOL_LOOP "1" : LV_SYMBOL_LOOP);

    // Volume (do not fight the finger).
    if (!vol_drag_) {
      if (lv_slider_get_value(vol_) != st.volume) lv_slider_set_value(vol_, st.volume, LV_ANIM_OFF);
      showVolume(st.volume);
    }

    // List / messages.
    const bool show_list = sn.sd && !sn.scanning && sn.count > 0;
    if (show_list) lv_obj_set_hidden(list_, false);
    else lv_obj_set_hidden(list_, true);
    if (show_list) {
      lv_obj_set_hidden(msg_, true);
    } else {
      lv_label_set_text(msg_, !sn.sd ? LV_SYMBOL_SD_CARD "  No microSD card"
                                     : sn.scanning ? "Scanning..." : "No tracks found\n(Music folder or card root)");
      lv_obj_set_hidden(msg_, false);
    }
    if (sn.version != version_ || sn.count != count_) {
      const bool track_changed = sn.current != current_;
      version_ = sn.version;
      count_ = sn.count;
      current_ = sn.current;
      lv_obj_set_size(spacer_, 1, (int32_t)(count_ ? count_ * kRowH : 1));
      layoutRows();
      if (track_changed && current_ >= 0 && !lv_obj_is_scrolling(list_)) revealCurrent();
    }
  }

  void showVolume(int v) {
    if (v == shown_vol_) return;
    shown_vol_ = v;
    char b[8];
    snprintf(b, sizeof b, "%d%%", v);
    lv_label_set_text(vol_pct_, b);
  }

  void revealCurrent() {
    const int32_t y = lv_obj_get_scroll_y(list_);
    const int32_t top = current_ * kRowH;
    if (top < y) lv_obj_scroll_to_y(list_, top, LV_ANIM_OFF);
    else if (top + kRowH > y + kListH) lv_obj_scroll_to_y(list_, top + kRowH - kListH, LV_ANIM_OFF);
    layoutRows();
  }

  // Binds the row widgets to the tracks that are currently in view.
  void layoutRows() {
    int32_t y = lv_obj_get_scroll_y(list_);
    if (y < 0) y = 0;
    const int first = (int)(y / kRowH);
    for (int k = 0; k < kRows; k++) {
      Row& r = rows_[k];
      const int idx = first + k;
      if (idx < 0 || (size_t)idx >= count_) {
        if (r.idx != -1) lv_obj_set_hidden(r.btn, true);
        r.idx = -1;
        continue;
      }
      const bool hl = idx == current_;
      if (r.idx != idx) {
        char name[64];
        svc_->displayName((size_t)idx, name, sizeof name);
        lv_label_set_text(r.lbl, name);
        lv_obj_set_pos(r.btn, 4, idx * kRowH + 1);
        lv_obj_set_hidden(r.btn, false);
        r.idx = idx;
        r.hl = !hl;                              // force the style update below
      }
      if (r.hl != hl) {
        lv_obj_set_style_bg_opa(r.btn, hl ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(r.btn, lv_palette_darken(LV_PALETTE_BLUE, 2), 0);
        r.hl = hl;
      }
    }
  }

  // ---- callbacks ----
  static MusicApp* self(lv_event_t* e) { return static_cast<MusicApp*>(lv_event_get_user_data(e)); }

  static void onTimer(lv_timer_t* t) { static_cast<MusicApp*>(lv_timer_get_user_data(t))->refresh(); }
  static void onListScroll(lv_event_t* e) { self(e)->layoutRows(); }

  static void onRow(lv_event_t* e) {
    Row* r = static_cast<Row*>(lv_event_get_user_data(e));
    if (r->idx >= 0 && r->owner->svc_) r->owner->svc_->playIndex((size_t)r->idx);
    r->owner->refresh();
  }

  static void onPlay(lv_event_t* e) { self(e)->svc_->togglePlay(); self(e)->refresh(); }
  static void onPrev(lv_event_t* e) { self(e)->svc_->prev(); self(e)->refresh(); }
  static void onNext(lv_event_t* e) { self(e)->svc_->next(); self(e)->refresh(); }
  static void onStop(lv_event_t* e) { self(e)->svc_->stop(); self(e)->refresh(); }
  static void onShuffle(lv_event_t* e) { self(e)->svc_->toggleShuffle(); self(e)->refresh(); }
  static void onRepeat(lv_event_t* e) { self(e)->svc_->cycleRepeat(); self(e)->refresh(); }

  static void onSeek(lv_event_t* e) {
    MusicApp* a = self(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
      a->seeking_ = true;
    } else if (code == LV_EVENT_VALUE_CHANGED && a->seeking_) {
      char t[16];
      fmtTime(t, sizeof t, (uint32_t)((uint64_t)lv_slider_get_value(a->seek_) * a->dur_ms_ / 1000));
      lv_label_set_text(a->elapsed_, t);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && a->seeking_) {
      a->seeking_ = false;
      if (a->audio_ && a->dur_ms_)
        a->audio_->seek((uint32_t)((uint64_t)lv_slider_get_value(a->seek_) * a->dur_ms_ / 1000));
    }
  }

  static void onVolume(lv_event_t* e) {
    MusicApp* a = self(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
      a->vol_drag_ = true;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
      const int v = lv_slider_get_value(a->vol_);
      if (a->audio_) a->audio_->setVolume((uint8_t)v);
      a->showVolume(v);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
      a->vol_drag_ = false;
    }
  }

  os::Context* ctx_ = nullptr;
  os::Audio* audio_ = nullptr;
  MusicService* svc_ = nullptr;
  lv_timer_t* timer_ = nullptr;

  lv_obj_t *title_ = nullptr, *index_ = nullptr, *elapsed_ = nullptr, *total_ = nullptr;
  lv_obj_t *seek_ = nullptr, *vol_ = nullptr, *vol_pct_ = nullptr, *msg_ = nullptr;
  lv_obj_t *btn_shuffle_ = nullptr, *btn_prev_ = nullptr, *btn_play_ = nullptr, *btn_stop_ = nullptr;
  lv_obj_t *btn_next_ = nullptr, *btn_repeat_ = nullptr;
  lv_obj_t *lbl_play_ = nullptr, *lbl_shuffle_ = nullptr, *lbl_repeat_ = nullptr;
  lv_obj_t *list_ = nullptr, *spacer_ = nullptr;
  Row rows_[kRows] = {};

  uint32_t version_ = ~0u;
  size_t count_ = 0;
  int current_ = -1;
  uint32_t dur_ms_ = 0;
  int shown_vol_ = -1;
  bool seeking_ = false, vol_drag_ = false;
  char last_title_[80] = {};
  char last_idx_[16] = {};
};

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "music",
    .name = "Music",
    .icon = nullptr,
    .glyph = LV_SYMBOL_AUDIO,
    .perms = os::AUDIO | os::SD,
    .psram_arena = 128 * 1024,     // playlist pool (32 KB) + entries + service state
    .sram_budget = 0,
    .background = true,
    .make = os::makeApp<MusicApp>,
    .make_service = os::makeService<MusicService>,
};

}  // namespace apps::music
