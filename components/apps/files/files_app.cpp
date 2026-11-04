// File explorer for the microSD card (browsing only).
//
// Memory model: the entry table lives in the app arena (std::pmr). The list is virtualised:
// only kPool row widgets exist, re-bound while scrolling, so a 512-entry folder costs the same
// as a 5-entry one. Directory handles are opened and closed inside load(); nothing stays open
// between events, so a card removal can never leave a dangling handle.
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory_resource>
#include <vector>

#include "os/app.h"

namespace apps::files {

namespace {

constexpr size_t kMaxEntries = 512;       // per directory
constexpr size_t kMaxPool = 48 * 1024;    // bytes of names per directory
constexpr size_t kPathCap = 512;
constexpr int kRowH = 36;
constexpr int kPool = 8;                  // row widgets: visible rows + slack
constexpr int kListY = 44;
constexpr int kListH = 264 - kListY;
constexpr int kW = 456;

enum Kind : uint8_t { Dir, Audio, Image, Text, Other };

struct Entry {
  uint32_t name_off;
  Kind kind;
};

bool extIs(const char* name, const char* const* list) {
  const char* dot = strrchr(name, '.');
  if (!dot || !dot[1]) return false;
  for (; *list; list++) {
    if (strcasecmp(dot + 1, *list) == 0) return true;
  }
  return false;
}

Kind kindOf(const char* name) {
  static const char* const audio[] = {"mp3", "wav", "flac", "ogg", "m4a", "aac", "opus", "wma", "mid", nullptr};
  static const char* const image[] = {"jpg", "jpeg", "png", "bmp", "gif", "webp", "svg", nullptr};
  static const char* const text[] = {"txt", "log", "md", "csv", "json", "ini", "cfg", "xml", "yaml",
                                     "yml", "c",   "h",  "cpp", "py",   "js",  "html", nullptr};
  if (extIs(name, audio)) return Audio;
  if (extIs(name, image)) return Image;
  if (extIs(name, text)) return Text;
  return Other;
}

const char* iconOf(Kind k) {
  switch (k) {
    case Dir: return LV_SYMBOL_DIRECTORY;
    case Audio: return LV_SYMBOL_AUDIO;
    case Image: return LV_SYMBOL_IMAGE;
    case Text: return LV_SYMBOL_EDIT;
    default: return LV_SYMBOL_FILE;
  }
}

lv_color_t iconColor(Kind k) {
  switch (k) {
    case Dir: return lv_palette_main(LV_PALETTE_AMBER);
    case Audio: return lv_palette_main(LV_PALETTE_PINK);
    case Image: return lv_palette_main(LV_PALETTE_GREEN);
    case Text: return lv_palette_main(LV_PALETTE_LIGHT_BLUE);
    default: return lv_palette_main(LV_PALETTE_GREY);
  }
}

// "123 B", "4.5 KB", "12.3 MB", "1.2 GB" (integer math only).
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

void formatTime(time_t t, bool with_time, char* out, size_t cap) {
  struct tm tmv;
  if (t < 86400 * 366 || !localtime_r(&t, &tmv)) {
    snprintf(out, cap, "-");
    return;
  }
  strftime(out, cap, with_time ? "%Y-%m-%d %H:%M" : "%Y-%m-%d", &tmv);
}

}  // namespace

class FilesApp final : public os::App {
 public:
  explicit FilesApp(std::pmr::memory_resource* mem) : entries_(mem), pool_(mem) {}

  void onCreate(os::Context& ctx) override {
    fs_ = ctx.sd();
    entries_.reserve(kMaxEntries);
    pool_.reserve(16 * 1024);
  }

  void onStart(lv_obj_t* root) override {
    root_ = root;
    buildEmptyView();
    buildCardView();
    timer_ = lv_timer_create(onTimer, 1000, this);
    reset();
  }

  void onEvent(const os::Msg& m) override {
    if (m.topic == os::Topic::Storage) reset();      // card inserted or removed: start over
  }

  void onStop() override {
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
  }

 private:
  // ---- UI construction -----------------------------------------------------------------
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

  void buildEmptyView() {
    empty_ = lv_obj_create(root_);
    plain(empty_);
    lv_obj_set_size(empty_, kW, 264);
    lv_obj_set_flex_flow(empty_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(empty_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(empty_, 8, 0);

    lv_obj_t* ic = makeLabel(empty_, &lv_font_montserrat_28, lv_palette_main(LV_PALETTE_GREY));
    lv_label_set_text(ic, LV_SYMBOL_SD_CARD);
    lv_obj_t* t = makeLabel(empty_, &lv_font_montserrat_20, lv_color_white());
    lv_label_set_text(t, "No microSD card");
    hint_ = makeLabel(empty_, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_obj_set_width(hint_, 360);
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint_, "Insert a FAT32 card. It is detected automatically.");

    lv_obj_t* btn = lv_button_create(empty_);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_REFRESH "  Refresh");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, onRefresh, LV_EVENT_CLICKED, this);
  }

  void buildCardView() {
    card_ = lv_obj_create(root_);
    plain(card_);
    lv_obj_set_size(card_, kW, 264);

    path_ = makeLabel(card_, &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_width(path_, kW);
    lv_label_set_long_mode(path_, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(path_, 0, 0);

    info_ = makeLabel(card_, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
    lv_obj_set_width(info_, kW);
    lv_label_set_long_mode(info_, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(info_, 0, 20);

    list_ = lv_obj_create(card_);
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
      lv_obj_set_style_bg_color(r.obj, lv_color_make(0x2A, 0x32, 0x40), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, LV_STATE_PRESSED);
      lv_obj_set_style_radius(r.obj, 6, 0);
      lv_obj_set_style_border_width(r.obj, 0, 0);
      lv_obj_add_event_cb(r.obj, onRow, LV_EVENT_CLICKED, this);

      r.icon = makeLabel(r.obj, &lv_font_montserrat_20, lv_color_white());
      lv_obj_align(r.icon, LV_ALIGN_LEFT_MID, 6, 0);
      r.name = makeLabel(r.obj, &lv_font_montserrat_14, lv_color_white());
      lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_size(r.name, 262, 18);
      lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 40, 0);
      r.size = makeLabel(r.obj, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 1));
      lv_obj_set_width(r.size, 70);
      lv_obj_set_style_text_align(r.size, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_align(r.size, LV_ALIGN_RIGHT_MID, -84, 0);
      r.date = makeLabel(r.obj, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
      lv_obj_set_width(r.date, 78);
      lv_obj_set_style_text_align(r.date, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_align(r.date, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    msg_ = makeLabel(card_, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
    lv_obj_set_width(msg_, kW);
    lv_obj_set_style_text_align(msg_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(msg_, 0, kListY + kListH / 2);
    lv_obj_set_hidden(msg_, true);
  }

  // ---- state ---------------------------------------------------------------------------
  // Back to the root (or the empty state when there is no card). Drops any open detail view.
  void reset() {
    closeDetail();
    const bool present = fs_ && fs_->mounted();
    has_card_ = present;
    if (!present) {
      entries_.clear();
      pool_.clear();
      lv_label_set_text(hint_, "Insert a FAT32 card. It is detected automatically.");
      lv_obj_set_hidden(card_, true);
      lv_obj_set_hidden(empty_, false);
      return;
    }
    lv_obj_set_hidden(empty_, true);
    lv_obj_set_hidden(card_, false);
    snprintf(path_buf_, sizeof path_buf_, "%s", fs_->mountPoint());
    root_len_ = strlen(path_buf_);
    updateInfo();
    load();
  }

  void updateInfo() {
    uint64_t total = 0, free_b = 0;
    char a[24] = "?", b[24] = "?";
    if (fs_->capacity(total, free_b)) {
      humanSize(free_b, a, sizeof a);
      humanSize(total, b, sizeof b);
    }
    snprintf(info_buf_, sizeof info_buf_, "%s free of %s", a, b);
    lv_label_set_text(info_, info_buf_);
  }

  void showPath() {
    // Show the tail of long paths so the current folder is always visible.
    const char* rel = path_buf_ + root_len_;
    char disp[80];
    if (!*rel) rel = "/";
    const size_t n = strlen(rel);
    constexpr size_t kMaxShown = 60;
    if (n <= kMaxShown) {
      snprintf(disp, sizeof disp, LV_SYMBOL_SD_CARD " %s", rel);
    } else {
      const char* tail = rel + (n - kMaxShown);
      while ((*tail & 0xC0) == 0x80) tail++;       // do not cut a UTF-8 sequence
      snprintf(disp, sizeof disp, LV_SYMBOL_SD_CARD " ...%s", tail);
    }
    lv_label_set_text(path_, disp);
  }

  bool atRoot() const { return path_buf_[root_len_] == '\0'; }

  // Reads the current directory into entries_ (sorted) and rebuilds the visible rows.
  void load() {
    entries_.clear();
    pool_.clear();
    truncated_ = false;
    bool ok = false;
    if (DIR* d = opendir(path_buf_)) {
      ok = true;
      while (struct dirent* de = readdir(d)) {
        const char* nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        const size_t len = strlen(nm) + 1;
        if (entries_.size() >= kMaxEntries || pool_.size() + len > kMaxPool) {
          truncated_ = true;
          break;
        }
        Entry e;
        e.name_off = (uint32_t)pool_.size();
        e.kind = (de->d_type == DT_DIR) ? Dir : kindOf(nm);
        pool_.insert(pool_.end(), nm, nm + len);
        entries_.push_back(e);
      }
      closedir(d);                                  // never keep the handle
    }
    const char* base = pool_.data();
    std::sort(entries_.begin(), entries_.end(), [base](const Entry& a, const Entry& b) {
      if ((a.kind == Dir) != (b.kind == Dir)) return a.kind == Dir;
      return strcasecmp(base + a.name_off, base + b.name_off) < 0;
    });

    showPath();
    updateInfo();
    const int total = rowCount();
    lv_obj_set_height(spacer_, total > 0 ? total * kRowH : 1);
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    for (auto& r : rows_) r.idx = -1;              // force a re-bind
    bindRows();

    if (!ok) {
      lv_label_set_text(msg_, fs_->mounted() ? "Cannot read this folder" : "Card removed");
    } else if (entries_.empty()) {
      lv_label_set_text(msg_, "Empty folder");
    } else if (truncated_) {
      char t[112];
      snprintf(t, sizeof t, "%s  |  first %u entries shown", info_buf_, (unsigned)entries_.size());
      lv_label_set_text(info_, t);
    }
    if (!ok || entries_.empty()) lv_obj_set_hidden(msg_, false);
    else lv_obj_set_hidden(msg_, true);
  }

  int rowCount() const { return (int)entries_.size() + (atRoot() ? 0 : 1); }

  // ---- virtual list --------------------------------------------------------------------
  struct Row {
    lv_obj_t *obj, *icon, *name, *size, *date;
    int idx = -1;
  };

  void bindRows() {
    const int total = rowCount();
    int first = lv_obj_get_scroll_y(list_) / kRowH;
    if (first < 0) first = 0;
    // Rows [first, first + kPool) are shown; row i always lives in slot i % kPool.
    for (int i = 0; i < kPool; i++) {
      const int idx = first + i;
      Row& r = rows_[idx % kPool];
      if (idx >= total) {
        if (r.idx != -2) {
          lv_obj_set_hidden(r.obj, true);
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
    lv_obj_set_hidden(r.obj, false);
    const bool up = !atRoot() && idx == 0;
    if (up) {
      lv_label_set_text(r.icon, LV_SYMBOL_UP);
      lv_obj_set_style_text_color(r.icon, lv_palette_main(LV_PALETTE_AMBER), 0);
      lv_label_set_text(r.name, ".. (up)");
      lv_label_set_text(r.size, "");
      lv_label_set_text(r.date, "");
      return;
    }
    const Entry& e = entries_[idx - (atRoot() ? 0 : 1)];
    const char* nm = pool_.data() + e.name_off;
    lv_label_set_text(r.icon, iconOf(e.kind));
    lv_obj_set_style_text_color(r.icon, iconColor(e.kind), 0);
    lv_label_set_text(r.name, nm);

    // Size and date are fetched lazily, only for the rows that are actually shown.
    struct stat st;
    char buf[24];
    if (childPath(nm) && stat(scratch_, &st) == 0) {
      if (e.kind == Dir) {
        lv_label_set_text(r.size, "");
      } else {
        humanSize((uint64_t)st.st_size, buf, sizeof buf);
        lv_label_set_text(r.size, buf);
      }
      formatTime(st.st_mtime, false, buf, sizeof buf);
      lv_label_set_text(r.date, buf);
    } else {
      lv_label_set_text(r.size, "");
      lv_label_set_text(r.date, "");
    }
  }

  // Builds "<current path>/<name>" in scratch_; false if it does not fit.
  bool childPath(const char* name) {
    const int n = snprintf(scratch_, sizeof scratch_, "%s/%s", path_buf_, name);
    return n > 0 && (size_t)n < sizeof scratch_;
  }

  // ---- navigation / detail -------------------------------------------------------------
  void goUp() {
    char* slash = strrchr(path_buf_, '/');
    if (slash && (size_t)(slash - path_buf_) >= root_len_) *slash = '\0';
    load();
  }

  void open(int idx) {
    if (!fs_->mounted()) {
      reset();
      return;
    }
    if (!atRoot() && idx == 0) {
      goUp();
      return;
    }
    const Entry& e = entries_[idx - (atRoot() ? 0 : 1)];
    const char* nm = pool_.data() + e.name_off;
    if (e.kind == Dir) {
      if (!childPath(nm)) return;                    // path too long
      memcpy(path_buf_, scratch_, strlen(scratch_) + 1);
      load();
    } else {
      showDetail(nm, e.kind);
    }
  }

  void showDetail(const char* name, Kind kind) {
    closeDetail();
    struct stat st;
    char size[40] = "?", exact[48] = "", when[24] = "-";
    if (childPath(name) && stat(scratch_, &st) == 0) {
      humanSize((uint64_t)st.st_size, size, sizeof size);
      snprintf(exact, sizeof exact, " (%llu bytes)", (unsigned long long)st.st_size);
      formatTime(st.st_mtime, true, when, sizeof when);
    }
    detail_ = lv_obj_create(root_);
    lv_obj_remove_style_all(detail_);
    lv_obj_set_size(detail_, kW, 264);
    lv_obj_set_style_bg_color(detail_, lv_color_make(0x16, 0x1B, 0x24), 0);
    lv_obj_set_style_bg_opa(detail_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(detail_, 10, 0);
    lv_obj_set_style_pad_all(detail_, 12, 0);
    lv_obj_set_scrollable(detail_, false);
    lv_obj_set_clickable(detail_, true);   // swallow taps on the list below

    lv_obj_t* ic = makeLabel(detail_, &lv_font_montserrat_28, iconColor(kind));
    lv_label_set_text(ic, iconOf(kind));
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* nm = makeLabel(detail_, &lv_font_montserrat_20, lv_color_white());
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(nm, kW - 24 - 44, 72);             // up to 3 lines, then an ellipsis
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 44, 0);
    lv_label_set_text(nm, name);

    lv_obj_t* l = makeLabel(detail_, &lv_font_montserrat_14, lv_palette_lighten(LV_PALETTE_GREY, 2));
    lv_obj_set_width(l, kW - 24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, 60);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 84);
    lv_label_set_text_fmt(l, "Size: %s%s\nModified: %s", size, exact, when);

    lv_obj_t* btn = lv_button_create(detail_);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, "Close");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, onCloseDetail, LV_EVENT_CLICKED, this);
  }

  void closeDetail() {
    if (detail_) lv_obj_delete(detail_);
    detail_ = nullptr;
  }

  // ---- callbacks -----------------------------------------------------------------------
  static void onTimer(lv_timer_t* t) {
    auto* self = static_cast<FilesApp*>(lv_timer_get_user_data(t));
    // Safety net in case a bus event was dropped.
    if ((self->fs_ && self->fs_->mounted()) != self->has_card_) self->reset();
  }

  static void onScroll(lv_event_t* e) {
    static_cast<FilesApp*>(lv_event_get_user_data(e))->bindRows();
  }

  static void onRow(lv_event_t* e) {
    auto* self = static_cast<FilesApp*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_target_obj(e);
    for (auto& r : self->rows_) {
      if (r.obj == target && r.idx >= 0) {
        self->open(r.idx);
        return;
      }
    }
  }

  static void onRefresh(lv_event_t* e) {
    auto* self = static_cast<FilesApp*>(lv_event_get_user_data(e));
    self->reset();
    if (!self->has_card_) lv_label_set_text(self->hint_, "Still no card detected. Detection runs every 1-2 s.");
  }

  static void onCloseDetail(lv_event_t* e) {
    static_cast<FilesApp*>(lv_event_get_user_data(e))->closeDetail();
  }

  os::Fs* fs_ = nullptr;
  std::pmr::vector<Entry> entries_;
  std::pmr::vector<char> pool_;
  bool truncated_ = false;
  bool has_card_ = false;

  char path_buf_[kPathCap] = {};
  char scratch_[kPathCap + 260] = {};
  char info_buf_[64] = {};
  size_t root_len_ = 0;

  lv_obj_t *root_ = nullptr, *empty_ = nullptr, *card_ = nullptr, *path_ = nullptr, *info_ = nullptr,
           *list_ = nullptr, *spacer_ = nullptr, *msg_ = nullptr, *hint_ = nullptr, *detail_ = nullptr;
  lv_timer_t* timer_ = nullptr;
  Row rows_[kPool];
};

static os::App* make(std::pmr::memory_resource& m) {
  void* p = m.allocate(sizeof(FilesApp), alignof(FilesApp));
  return new (p) FilesApp(&m);
}

extern const os::Manifest kManifest;
const os::Manifest kManifest = {
    .id = "files",
    .name = "Files",
    .icon = nullptr,
    .glyph = LV_SYMBOL_DIRECTORY,
    .perms = os::SD,
    .psram_arena = 262144,
    .sram_budget = 0,
    .background = false,
    .make = make,
    .make_service = nullptr,
};

}  // namespace apps::files
