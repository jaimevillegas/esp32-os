// Voice recorder UI. The engine (capture task, WAV writer) lives in the firmware and is the
// source of truth: this class only shows os::Recorder::status() and forwards user actions, so
// leaving the app while recording keeps the recording alive and reopening it shows it again.
//
// Layout inside the 456 x 264 content area (all positions absolute):
//   y   0.. 72  record/stop button 72x72 at x 0
//   y   0.. 32  elapsed time (28 pt) at x 92, status text right-aligned at x 230..456
//   y  40.. 52  level meter (lv_bar), x 92, 364 px wide
//   y  58.. 76  target file name;  y 76.. 94  free space / remaining time
//   y  98..220  scrollable list of recordings (virtualised, kPool row widgets, 34 px rows)
//   y 224..264  Play/Stop, Delete, playback position
//
// Memory: the file names live in the app arena (std::pmr), fixed 32-byte slots, at most 128
// files (the newest ones when there are more). No directory handle outlives loadList().
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <vector>

#include "os/app.h"

namespace apps::recorder {

namespace {

constexpr int kW = 456;
constexpr int kH = 264;
constexpr int kMaxFiles = 128;
constexpr size_t kNameCap = 32;             // bytes per name slot, including the NUL
constexpr int kRowH = 34;
constexpr int kPool = 6;                    // row widgets: 3.6 visible rows + slack
constexpr int kListY = 98;
constexpr int kListH = 122;
constexpr int kBarY = 224;
constexpr uint32_t kBytesPerSec = 32000;    // 16 kHz, 16 bit, mono
constexpr uint32_t kTickMs = 100;           // level meter / clock refresh while visible
constexpr uint32_t kSpaceEveryTicks = 50;   // free space is re-read every 5 s

inline lv_color_t panelColor() { return lv_color_hex(0x161B24); }

void fmtClock(char* out, size_t cap, uint32_t ms) {
  const uint32_t s = ms / 1000;
  if (s >= 3600) snprintf(out, cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
  else snprintf(out, cap, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void humanSize(uint64_t b, char* out, size_t cap) {
  static const char* const unit[] = {"B", "KB", "MB", "GB", "TB"};
  int u = 0;
  uint64_t whole = b, rem = 0;
  while (whole >= 1024 && u < 4) {
    rem = whole % 1024;
    whole /= 1024;
    u++;
  }
  if (u == 0) snprintf(out, cap, "%u B", (unsigned)whole);
  else snprintf(out, cap, "%u.%u %s", (unsigned)whole, (unsigned)(rem * 10 / 1024), unit[u]);
}

const char* baseName(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

}  // namespace

class RecorderApp final : public os::App {
 public:
  explicit RecorderApp(std::pmr::memory_resource* mem) : pool_(mem), order_(mem) {}

  void onCreate(os::Context& ctx) override {
    rec_ = ctx.recorder();
    audio_ = ctx.audio();
    sd_ = ctx.sd();
    pool_.reserve(kMaxFiles * kNameCap);
    order_.reserve(kMaxFiles);
    if (sd_) snprintf(dir_, sizeof dir_, "%s/Recordings", sd_->mountPoint());
  }

  void onStart(lv_obj_t* root) override {
    root_ = root;
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    buildNoCard();
    buildBody();
    shown_rec_ = false;
    has_card_ = false;
    sync();
  }

  void onResume() override {
    if (!timer_) timer_ = lv_timer_create(onTimer, kTickMs, this);   // only while visible
    sync();
  }

  void onPause() override { stopTimer(); }

  void onStop() override { stopTimer(); }

  void onEvent(const os::Msg& m) override {
    if (m.topic == os::Topic::Recorder || m.topic == os::Topic::Audio || m.topic == os::Topic::Storage) sync();
  }

 private:
  // ---- small widget helpers ------------------------------------------------------------
  static lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
  }

  static void plain(lv_obj_t* o) {
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(o, false);
  }

  static void setShown(lv_obj_t* o, bool shown) {
    lv_obj_set_hidden(o, !shown);
  }

  static void setEnabled(lv_obj_t* o, bool on) {
    if (on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
  }

  lv_obj_t* makeButton(lv_obj_t* parent, int x, int y, int w, int h, const char* text, lv_event_cb_t cb) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    return b;
  }

  // ---- UI construction -----------------------------------------------------------------
  void buildNoCard() {
    nocard_ = lv_obj_create(root_);
    plain(nocard_);
    lv_obj_set_size(nocard_, kW, kH);
    lv_obj_set_flex_flow(nocard_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nocard_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(nocard_, 8, 0);
    lv_label_set_text(makeLabel(nocard_, &lv_font_montserrat_28, lv_palette_main(LV_PALETTE_GREY)), LV_SYMBOL_SD_CARD);
    lv_label_set_text(makeLabel(nocard_, &lv_font_montserrat_20, lv_color_white()), "No microSD card");
    lv_obj_t* h = makeLabel(nocard_, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_obj_set_width(h, 360);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(h, "Insert a FAT32 card to record. It is detected automatically.");
  }

  void buildBody() {
    body_ = lv_obj_create(root_);
    plain(body_);
    lv_obj_set_size(body_, kW, kH);

    // Record / stop button: a red disc with a white dot (idle) or a stop square (recording).
    rec_btn_ = lv_button_create(body_);
    lv_obj_set_size(rec_btn_, 72, 72);
    lv_obj_set_pos(rec_btn_, 0, 0);
    lv_obj_set_style_radius(rec_btn_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_btn_, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(rec_btn_, onRecClick, LV_EVENT_CLICKED, this);
    rec_dot_ = lv_obj_create(rec_btn_);
    plain(rec_dot_);
    lv_obj_set_size(rec_dot_, 26, 26);
    lv_obj_set_style_radius(rec_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_dot_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(rec_dot_, LV_OPA_COVER, 0);
    lv_obj_set_clickable(rec_dot_, false);
    lv_obj_center(rec_dot_);
    rec_stop_ = lv_obj_create(rec_btn_);
    plain(rec_stop_);
    lv_obj_set_size(rec_stop_, 24, 24);
    lv_obj_set_style_radius(rec_stop_, 3, 0);
    lv_obj_set_style_bg_color(rec_stop_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(rec_stop_, LV_OPA_COVER, 0);
    lv_obj_set_clickable(rec_stop_, false);
    lv_obj_center(rec_stop_);
    setShown(rec_stop_, false);

    time_ = makeLabel(body_, &lv_font_montserrat_28, lv_color_white());
    lv_obj_set_pos(time_, 92, 0);
    lv_label_set_text(time_, "00:00");

    status_ = makeLabel(body_, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
    lv_obj_set_size(status_, 226, 18);
    lv_obj_set_pos(status_, 230, 8);
    lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_MODE_DOTS);

    level_ = lv_bar_create(body_);
    lv_obj_set_size(level_, 364, 12);
    lv_obj_set_pos(level_, 92, 40);
    lv_bar_set_range(level_, 0, 100);
    lv_obj_set_style_bg_color(level_, lv_color_make(0x2A, 0x32, 0x40), 0);
    lv_obj_set_style_bg_color(level_, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);

    file_ = makeLabel(body_, &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_size(file_, 364, 18);
    lv_obj_set_pos(file_, 92, 58);
    lv_label_set_long_mode(file_, LV_LABEL_LONG_MODE_DOTS);

    space_ = makeLabel(body_, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_obj_set_size(space_, 364, 18);
    lv_obj_set_pos(space_, 92, 76);
    lv_label_set_long_mode(space_, LV_LABEL_LONG_MODE_DOTS);

    // ---- list of recordings ----
    list_ = lv_obj_create(body_);
    plain(list_);
    lv_obj_set_pos(list_, 0, kListY);
    lv_obj_set_size(list_, kW, kListH);
    lv_obj_set_scrollable(list_, true);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(list_, onScroll, LV_EVENT_SCROLL, this);

    spacer_ = lv_obj_create(list_);
    plain(spacer_);
    lv_obj_set_clickable(spacer_, false);
    lv_obj_set_pos(spacer_, 0, 0);
    lv_obj_set_size(spacer_, 1, 1);

    for (int i = 0; i < kPool; i++) {
      Row& r = rows_[i];
      r.obj = lv_obj_create(list_);
      plain(r.obj);
      lv_obj_set_clickable(r.obj, true);
      lv_obj_set_hidden(r.obj, true);
      lv_obj_set_size(r.obj, kW, kRowH);
      lv_obj_set_style_radius(r.obj, 6, 0);
      lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(r.obj, lv_color_make(0x2A, 0x32, 0x40), LV_STATE_PRESSED);
      lv_obj_add_event_cb(r.obj, onRow, LV_EVENT_CLICKED, this);

      r.icon = makeLabel(r.obj, &lv_font_montserrat_20, lv_palette_main(LV_PALETTE_PINK));
      lv_obj_align(r.icon, LV_ALIGN_LEFT_MID, 8, 0);
      r.name = makeLabel(r.obj, &lv_font_montserrat_20, lv_color_white());
      lv_obj_set_size(r.name, 230, 24);
      lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 40, 0);
      r.info = makeLabel(r.obj, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 1));
      lv_obj_set_width(r.info, 160);
      lv_obj_set_style_text_align(r.info, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_align(r.info, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    empty_ = makeLabel(body_, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
    lv_obj_set_width(empty_, kW);
    lv_obj_set_style_text_align(empty_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(empty_, 0, kListY + kListH / 2 - 8);
    lv_label_set_text(empty_, "No recordings yet");

    // ---- action bar ----
    play_btn_ = makeButton(body_, 0, kBarY, 130, 40, LV_SYMBOL_PLAY "  Play", onPlay);
    play_lbl_ = lv_obj_get_child(play_btn_, 0);
    del_btn_ = makeButton(body_, 138, kBarY, 130, 40, LV_SYMBOL_TRASH "  Delete", onDelete);
    pos_ = makeLabel(body_, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_obj_set_size(pos_, 176, 18);
    lv_obj_set_pos(pos_, 280, kBarY + 11);
    lv_obj_set_style_text_align(pos_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(pos_, "");

    // ---- delete confirmation (modal, hidden until needed) ----
    confirm_ = lv_obj_create(root_);
    lv_obj_remove_style_all(confirm_);
    lv_obj_set_size(confirm_, kW, kH);
    lv_obj_set_style_bg_color(confirm_, panelColor(), 0);
    lv_obj_set_style_bg_opa(confirm_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(confirm_, 10, 0);
    lv_obj_set_scrollable(confirm_, false);
    lv_obj_set_clickable(confirm_, true);       // swallows taps on the screen below
    lv_obj_t* q = makeLabel(confirm_, &lv_font_montserrat_20, lv_color_white());
    lv_obj_set_width(q, kW - 40);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 56);
    lv_label_set_text(q, "Delete this recording?");
    confirm_name_ = makeLabel(confirm_, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 2));
    lv_obj_set_width(confirm_name_, kW - 40);
    lv_obj_set_style_text_align(confirm_name_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(confirm_name_, LV_ALIGN_TOP_MID, 0, 96);
    lv_label_set_text(confirm_name_, "");
    lv_obj_t* no = makeButton(confirm_, 60, 160, 150, 44, "Cancel", onConfirmNo);
    (void)no;
    lv_obj_t* yes = makeButton(confirm_, 246, 160, 150, 44, LV_SYMBOL_TRASH "  Delete", onConfirmYes);
    lv_obj_set_style_bg_color(yes, lv_palette_main(LV_PALETTE_RED), 0);
    setShown(confirm_, false);
  }

  void stopTimer() {
    if (timer_) lv_timer_delete(timer_);               // timers are not children of the root
    timer_ = nullptr;
  }

  // ---- state ---------------------------------------------------------------------------
  const char* nameAt(int i) const { return pool_.data() + (size_t)order_[(size_t)i] * kNameCap; }
  int fileCount() const { return (int)order_.size(); }

  // Full refresh from the engine state: card, recorder, list, buttons.
  void sync() {
    if (!body_) return;
    const bool card = sd_ && sd_->mounted();
    os::RecStatus rs{};
    if (rec_) rec_->status(rs);
    const bool was_rec = shown_rec_;
    const bool card_changed = card != has_card_;
    has_card_ = card;
    setShown(nocard_, !card);
    setShown(body_, card);
    if (!card) {
      setShown(confirm_, false);
      shown_rec_ = rs.recording;
      return;
    }
    shown_rec_ = rs.recording;
    if (!rs.recording) stopping_ = false;
    else msg_[0] = 0;

    if (card_changed || was_rec != rs.recording || list_dirty_) {
      list_dirty_ = false;
      const bool finished = was_rec && !rs.recording && !rs.error && rs.path[0];
      loadList(finished ? baseName(rs.path) : nullptr);
    } else {
      rebindAll();                                   // playing indicator may have changed
    }
    rs_ = rs;
    updateSpace();
    updateLive(true);
    updateButtons();
  }

  void tick() {
    os::RecStatus rs{};
    if (rec_) rec_->status(rs);
    const bool card = sd_ && sd_->mounted();
    if (card != has_card_ || rs.recording != shown_rec_) {   // a bus event was dropped
      sync();
      return;
    }
    rs_ = rs;
    updateLive(false);
    if (++ticks_ >= kSpaceEveryTicks) {
      ticks_ = 0;
      updateSpace();
    }
  }

  // Elapsed time, level meter, status text, playback position. Writes to LVGL only on change.
  void updateLive(bool force) {
    const bool rec = rs_.recording;
    char t[24];
    fmtClock(t, sizeof t, rec ? rs_.elapsed_ms : 0);
    if (force || strcmp(t, last_time_) != 0) {
      snprintf(last_time_, sizeof last_time_, "%s", t);
      lv_label_set_text(time_, t);
    }
    const int lvl = rec ? rs_.level : 0;
    if (force || lvl != last_level_) {
      last_level_ = lvl;
      lv_bar_set_value(level_, lvl, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(level_, lvl > 90 ? lv_palette_main(LV_PALETTE_RED) : lv_palette_main(LV_PALETTE_GREEN),
                                LV_PART_INDICATOR);
    }

    const char* st;
    lv_color_t col = lv_palette_main(LV_PALETTE_GREY);
    if (rec && stopping_) {
      st = "Saving...";
    } else if (rec) {
      st = LV_SYMBOL_AUDIO "  Recording";
      col = lv_palette_main(LV_PALETTE_RED);
    } else if (rs_.error) {
      st = "Recording failed (card removed or full)";
      col = lv_palette_main(LV_PALETTE_ORANGE);
    } else if (msg_[0]) {
      st = msg_;
      col = lv_palette_main(LV_PALETTE_ORANGE);
    } else {
      st = "Ready";
    }
    if (force || strcmp(st, last_status_) != 0) {
      snprintf(last_status_, sizeof last_status_, "%s", st);
      lv_label_set_text(status_, st);
      lv_obj_set_style_text_color(status_, col, 0);
    }

    char f[48];
    if (rec) snprintf(f, sizeof f, "%s", baseName(rs_.path));
    else snprintf(f, sizeof f, "Next: rec_%04u.wav", next_index_);
    if (force || strcmp(f, last_file_) != 0) {
      snprintf(last_file_, sizeof last_file_, "%s", f);
      lv_label_set_text(file_, f);
    }

    // Playback position of the selected recording.
    char p[40] = "";
    if (audio_ && sel_ >= 0 && isPlaying(sel_)) {
      os::AudioStatus a;
      audio_->status(a);
      char c1[16], c2[16];
      fmtClock(c1, sizeof c1, a.pos_ms);
      fmtClock(c2, sizeof c2, a.dur_ms);
      snprintf(p, sizeof p, "%s / %s", c1, c2);
    }
    if (force || strcmp(p, last_pos_) != 0) {
      snprintf(last_pos_, sizeof last_pos_, "%s", p);
      lv_label_set_text(pos_, p);
    }
  }

  void updateSpace() {
    uint64_t total = 0, free_b = 0;
    char s[72];
    if (sd_ && sd_->capacity(total, free_b)) {
      char a[24];
      humanSize(free_b, a, sizeof a);
      const uint32_t secs = (uint32_t)std::min<uint64_t>(free_b / kBytesPerSec, 0xFFFFFFFFu);
      if (secs >= 3600) snprintf(s, sizeof s, "%s free  |  about %u h %02u min left", a, (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60));
      else snprintf(s, sizeof s, "%s free  |  about %u min left", a, (unsigned)(secs / 60));
    } else {
      snprintf(s, sizeof s, "Free space unknown");
    }
    lv_label_set_text(space_, s);
  }

  void updateButtons() {
    const bool rec = rs_.recording;
    setShown(rec_dot_, !rec);
    setShown(rec_stop_, rec);
    setEnabled(rec_btn_, !stopping_);
    const bool have = sel_ >= 0 && sel_ < fileCount();
    const bool playing = have && isPlaying(sel_);
    lv_label_set_text(play_lbl_, playing ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_PLAY "  Play");
    setEnabled(play_btn_, have && !rec && audio_);
    setEnabled(del_btn_, have && !rec);
    setShown(empty_, fileCount() == 0);
  }

  bool isPlaying(int idx) const {
    if (!audio_ || idx < 0 || idx >= fileCount()) return false;
    os::AudioStatus a;
    audio_->status(a);
    if (a.state != os::PlayState::Playing && a.state != os::PlayState::Paused) return false;
    char stem[kNameCap];
    snprintf(stem, sizeof stem, "%s", nameAt(idx));
    char* dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    return strcmp(a.title, stem) == 0;
  }

  // ---- list ----------------------------------------------------------------------------
  struct Row {
    lv_obj_t *obj, *icon, *name, *info;
    int idx = -1;
  };

  // Reads the recordings folder (newest first). `select` is the file name to select afterwards.
  void loadList(const char* select) {
    char keep[kNameCap] = "";
    if (select) snprintf(keep, sizeof keep, "%s", select);
    else if (sel_ >= 0 && sel_ < fileCount()) snprintf(keep, sizeof keep, "%s", nameAt(sel_));

    pool_.assign((size_t)kMaxFiles * kNameCap, 0);
    order_.clear();
    int used = 0;
    unsigned hi = 0;
    if (DIR* d = opendir(dir_)) {
      while (struct dirent* de = readdir(d)) {
        const char* nm = de->d_name;
        const size_t len = strlen(nm);
        if (len < 5 || len >= kNameCap || strcasecmp(nm + len - 4, ".wav") != 0) continue;
        unsigned idx;
        if (strncasecmp(nm, "rec_", 4) == 0 && sscanf(nm + 4, "%u", &idx) == 1 && idx > hi) hi = idx;
        int slot;
        if (used < kMaxFiles) {
          slot = used++;
        } else {                                      // full: the new name replaces the oldest one
          slot = 0;
          for (int i = 1; i < kMaxFiles; i++) {
            if (strcasecmp(slotName(i), slotName(slot)) < 0) slot = i;
          }
          if (strcasecmp(nm, slotName(slot)) <= 0) continue;
        }
        memcpy(pool_.data() + (size_t)slot * kNameCap, nm, len + 1);
      }
      closedir(d);                                    // never keep the handle
    }
    next_index_ = hi + 1;
    for (int i = 0; i < used; i++) order_.push_back((uint8_t)i);
    const char* base = pool_.data();
    std::sort(order_.begin(), order_.end(), [base](uint8_t a, uint8_t b) {
      return strcasecmp(base + (size_t)a * kNameCap, base + (size_t)b * kNameCap) > 0;
    });

    sel_ = -1;
    for (int i = 0; i < fileCount() && keep[0]; i++) {
      if (strcmp(nameAt(i), keep) == 0) sel_ = i;
    }
    lv_obj_set_height(spacer_, fileCount() > 0 ? fileCount() * kRowH : 1);
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    rebindAll();
  }

  const char* slotName(int slot) const { return pool_.data() + (size_t)slot * kNameCap; }

  void rebindAll() {
    for (auto& r : rows_) r.idx = -1;
    bindRows();
  }

  void bindRows() {
    const int total = fileCount();
    int first = lv_obj_get_scroll_y(list_) / kRowH;
    if (first < 0) first = 0;
    for (int i = 0; i < kPool; i++) {                 // row i always lives in slot i % kPool
      const int idx = first + i;
      Row& r = rows_[idx % kPool];
      if (idx >= total) {
        if (r.idx != -2) {
          setShown(r.obj, false);
          r.idx = -2;
        }
        continue;
      }
      if (r.idx == idx) continue;
      r.idx = idx;
      bindRow(r, idx);
    }
  }

  void bindRow(Row& r, int idx) {
    lv_obj_set_pos(r.obj, 0, idx * kRowH);
    setShown(r.obj, true);
    const char* nm = nameAt(idx);
    lv_label_set_text(r.name, nm);
    lv_label_set_text(r.icon, isPlaying(idx) ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO);
    lv_obj_set_style_bg_color(r.obj, idx == sel_ ? lv_color_make(0x2B, 0x4A, 0x7A) : panelColor(), 0);

    // Size (and so duration) only for the rows that are shown.
    struct stat st;
    char p[96], buf[48] = "";
    snprintf(p, sizeof p, "%s/%s", dir_, nm);
    if (stat(p, &st) == 0) {
      char sz[24], tm[16];
      humanSize((uint64_t)st.st_size, sz, sizeof sz);
      const uint32_t secs = st.st_size > 44 ? (uint32_t)((uint64_t)(st.st_size - 44) / kBytesPerSec) : 0;
      snprintf(tm, sizeof tm, "%u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
      snprintf(buf, sizeof buf, "%s   %s", tm, sz);
    }
    lv_label_set_text(r.info, buf);
  }

  // ---- actions -------------------------------------------------------------------------
  bool selPath(char* out, size_t cap) const {
    if (sel_ < 0 || sel_ >= fileCount()) return false;
    const int n = snprintf(out, cap, "%s/%s", dir_, nameAt(sel_));
    return n > 0 && (size_t)n < cap;
  }

  void doRecord() {
    if (!rec_) return;
    if (rs_.recording) {
      stopping_ = true;
      rec_->stop();
      updateLive(true);
      updateButtons();
      return;
    }
    msg_[0] = 0;
    if (!rec_->start(dir_)) {
      os::RecStatus rs{};
      rec_->status(rs);
      if (!rs.error) snprintf(msg_, sizeof msg_, "Cannot start recording");
    }
    sync();
  }

  void doPlay() {
    char p[96];
    if (!audio_ || rs_.recording || !selPath(p, sizeof p)) return;
    if (isPlaying(sel_)) audio_->stop();
    else if (!audio_->play(p)) snprintf(msg_, sizeof msg_, "Cannot play this file");
    sync();
  }

  void askDelete() {
    if (sel_ < 0 || sel_ >= fileCount() || rs_.recording) return;
    lv_label_set_text(confirm_name_, nameAt(sel_));
    setShown(confirm_, true);
  }

  void doDelete() {
    setShown(confirm_, false);
    char p[96];
    if (!selPath(p, sizeof p) || rs_.recording) return;
    if (isPlaying(sel_) && audio_) audio_->stop();
    if (unlink(p) != 0) snprintf(msg_, sizeof msg_, "Cannot delete the file");
    int keep = sel_;
    loadList("");                                     // reload, nothing selected
    if (fileCount() > 0) sel_ = keep < fileCount() ? keep : fileCount() - 1;
    rebindAll();
    sync();
  }

  // ---- callbacks -----------------------------------------------------------------------
  static RecorderApp* self(lv_event_t* e) { return static_cast<RecorderApp*>(lv_event_get_user_data(e)); }
  static void onTimer(lv_timer_t* t) { static_cast<RecorderApp*>(lv_timer_get_user_data(t))->tick(); }
  static void onScroll(lv_event_t* e) { self(e)->bindRows(); }
  static void onRecClick(lv_event_t* e) { self(e)->doRecord(); }
  static void onPlay(lv_event_t* e) { self(e)->doPlay(); }
  static void onDelete(lv_event_t* e) { self(e)->askDelete(); }
  static void onConfirmNo(lv_event_t* e) { setShown(self(e)->confirm_, false); }
  static void onConfirmYes(lv_event_t* e) { self(e)->doDelete(); }

  static void onRow(lv_event_t* e) {
    RecorderApp* a = self(e);
    lv_obj_t* target = lv_event_get_target_obj(e);
    for (auto& r : a->rows_) {
      if (r.obj == target && r.idx >= 0) {
        a->sel_ = (a->sel_ == r.idx) ? -1 : r.idx;
        a->rebindAll();
        a->updateLive(true);
        a->updateButtons();
        return;
      }
    }
  }

  os::Recorder* rec_ = nullptr;
  os::Audio* audio_ = nullptr;
  os::Fs* sd_ = nullptr;
  std::pmr::vector<char> pool_;                       // kMaxFiles name slots
  std::pmr::vector<uint8_t> order_;                   // slot indices, newest first
  char dir_[64] = {};
  unsigned next_index_ = 1;
  int sel_ = -1;
  bool has_card_ = false, shown_rec_ = false, stopping_ = false, list_dirty_ = false;
  uint32_t ticks_ = 0;
  os::RecStatus rs_{};
  char msg_[40] = {};
  char last_time_[24] = {}, last_status_[48] = {}, last_file_[48] = {}, last_pos_[40] = {};
  int last_level_ = -1;

  lv_obj_t *root_ = nullptr, *nocard_ = nullptr, *body_ = nullptr, *rec_btn_ = nullptr, *rec_dot_ = nullptr,
           *rec_stop_ = nullptr, *time_ = nullptr, *status_ = nullptr, *level_ = nullptr, *file_ = nullptr,
           *space_ = nullptr, *list_ = nullptr, *spacer_ = nullptr, *empty_ = nullptr, *play_btn_ = nullptr,
           *play_lbl_ = nullptr, *del_btn_ = nullptr, *pos_ = nullptr, *confirm_ = nullptr,
           *confirm_name_ = nullptr;
  lv_timer_t* timer_ = nullptr;
  Row rows_[kPool];
};

static os::App* make(std::pmr::memory_resource& m) {
  void* p = m.allocate(sizeof(RecorderApp), alignof(RecorderApp));
  return new (p) RecorderApp(&m);
}

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "recorder",
    .name = "Recorder",
    .icon = nullptr,
    .glyph = LV_SYMBOL_AUDIO,
    .perms = os::AUDIO | os::SD,
    .psram_arena = 131072,
    .sram_budget = 0,
    .background = false,
    .make = make,
    .make_service = nullptr,
};

}  // namespace apps::recorder
