// Bluetooth page: on/off, scan, connect, connected devices and bonded devices (BLE only).
//
// The device list is virtualised: a fixed pool of kPool rows is reused while scrolling (item i
// is shown by row i % kPool), so the widget count does not depend on the number of results.
#include <cstdio>
#include <cstring>

#include "page.h"

namespace apps::settings {

namespace {

constexpr int kMaxDev = 40;      // snapshot size (32 scan results + bonded)
constexpr int kMaxConn = 2;
constexpr int kPool = 8;         // list rows alive
constexpr int kRowH = 40;        // pitch; the row itself is kRowH - 4 high
constexpr uint32_t kScanMs = 10000;
constexpr uint32_t kTickMs = 600;
constexpr int kMsgTicks = 6;

lv_color_t colRow() { return lv_color_make(0x1C, 0x21, 0x28); }
lv_color_t colRowPressed() { return lv_color_make(0x2A, 0x32, 0x3D); }
lv_color_t colDim() { return lv_color_make(0x3A, 0x42, 0x4D); }
lv_color_t colAccent() { return lv_palette_main(LV_PALETTE_LIGHT_BLUE); }

void plain(lv_obj_t* o) {        // a decoration: no input, no scrolling
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

void fmtAddr(const os::BleDevice& d, char* out, size_t cap) {
  snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", d.addr[5], d.addr[4], d.addr[3], d.addr[2],
           d.addr[1], d.addr[0]);
}

void fmtName(const os::BleDevice& d, char* out, size_t cap) {
  if (d.name[0]) {
    snprintf(out, cap, "%s", d.name);
  } else {
    fmtAddr(d, out, cap);
  }
}

int rssiBars(const os::BleDevice& d) {
  if (d.rssi <= -127) return 0;
  if (d.rssi >= -55) return 4;
  if (d.rssi >= -67) return 3;
  if (d.rssi >= -80) return 2;
  return 1;
}

}  // namespace

class BluetoothPage final : public Page {
 public:
  const char* title() const override { return "Bluetooth"; }

  void build(lv_obj_t* body, lv_obj_t*, os::Context& ctx) override {
    ble_ = ctx.ble();
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);

    // ---- top row: status, spinner, Scan/Stop, switch ----
    lv_obj_t* top = lv_obj_create(body);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 36);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);

    status_ = lv_label_create(top);
    lv_obj_set_flex_grow(status_, 1);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(status_, lv_color_white(), 0);

    spinHolder_ = lv_obj_create(top);
    plain(spinHolder_);
    lv_obj_set_size(spinHolder_, 26, 26);

    scanBtn_ = lv_button_create(top);
    lv_obj_set_size(scanBtn_, 74, 32);
    scanLbl_ = lv_label_create(scanBtn_);
    lv_label_set_text(scanLbl_, "Scan");
    lv_obj_center(scanLbl_);
    lv_obj_add_event_cb(scanBtn_, onScan, LV_EVENT_CLICKED, this);

    sw_ = lv_switch_create(top);
    lv_obj_set_size(sw_, 52, 28);
    lv_obj_add_event_cb(sw_, onSwitch, LV_EVENT_VALUE_CHANGED, this);

    // ---- connected devices ----
    connBox_ = lv_obj_create(body);
    lv_obj_remove_style_all(connBox_);
    lv_obj_set_size(connBox_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(connBox_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(connBox_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(connBox_, 4, 0);
    for (int k = 0; k < kMaxConn; k++) buildConnRow(k);
    lv_obj_add_flag(connBox_, LV_OBJ_FLAG_HIDDEN);

    // ---- scanned and bonded devices ----
    list_ = lv_obj_create(body);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, lv_pct(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(list_, onScroll, LV_EVENT_SCROLL, this);
    sizer_ = lv_obj_create(list_);
    plain(sizer_);
    lv_obj_set_size(sizer_, 1, 1);
    for (int p = 0; p < kPool; p++) buildRow(p);
    empty_ = lv_label_create(list_);
    lv_obj_set_style_text_color(empty_, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_center(empty_);

    for (int p = 0; p < kPool; p++) rowItem_[p] = -1;
    timer_ = lv_timer_create(onTimer, kTickMs, this);
    refresh(true);
  }

  void onEvent(const os::Msg& m) override {
    if (m.topic == os::Topic::Ble) refresh(false);
  }

  void teardown() override {
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    ble_ = nullptr;
  }

 private:
  // ---- construction ----
  void buildConnRow(int k) {
    lv_obj_t* r = lv_obj_create(connBox_);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 40);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(r, colRow(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 6, 0);
    connRow_[k] = r;

    connName_[k] = lv_label_create(r);
    lv_obj_set_flex_grow(connName_[k], 1);
    lv_label_set_long_mode(connName_[k], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(connName_[k], lv_color_white(), 0);

    connBatt_[k] = lv_label_create(r);
    lv_obj_set_style_text_color(connBatt_[k], lv_palette_lighten(LV_PALETTE_GREEN, 1), 0);

    lv_obj_t* d = lv_button_create(r);
    lv_obj_set_size(d, 92, 30);
    lv_obj_set_user_data(d, (void*)(uintptr_t)k);
    lv_obj_add_event_cb(d, onDisconnect, LV_EVENT_CLICKED, this);
    lv_obj_t* dl = lv_label_create(d);
    lv_label_set_text(dl, "Disconnect");
    lv_obj_center(dl);

    lv_obj_t* f = lv_button_create(r);
    lv_obj_set_size(f, 64, 30);
    lv_obj_set_style_bg_color(f, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_obj_set_user_data(f, (void*)(uintptr_t)k);
    lv_obj_add_event_cb(f, onForgetConn, LV_EVENT_CLICKED, this);
    lv_obj_t* fl = lv_label_create(f);
    lv_label_set_text(fl, "Forget");
    lv_obj_center(fl);
  }

  void buildRow(int p) {
    lv_obj_t* r = lv_obj_create(list_);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), kRowH - 4);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_style_bg_color(r, colRow(), 0);
    lv_obj_set_style_bg_color(r, colRowPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(r, (void*)(uintptr_t)p);
    lv_obj_add_event_cb(r, onRow, LV_EVENT_CLICKED, this);
    row_[p] = r;

    rowName_[p] = lv_label_create(r);
    lv_obj_set_flex_grow(rowName_[p], 1);
    lv_label_set_long_mode(rowName_[p], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(rowName_[p], lv_color_white(), 0);

    lv_obj_t* bars = lv_obj_create(r);
    plain(bars);
    lv_obj_set_size(bars, 22, 16);
    for (int i = 0; i < 4; i++) {
      lv_obj_t* b = lv_obj_create(bars);
      plain(b);
      const int h = 4 + i * 4;
      lv_obj_set_size(b, 4, h);
      lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 6, 0);
      lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(b, colDim(), 0);
      bar_[p][i] = b;
    }

    // Fixed-size tail so the bars stay aligned whether or not the device is bonded.
    lv_obj_t* tail = lv_obj_create(r);
    plain(tail);
    lv_obj_set_size(tail, 66, 30);

    lock_[p] = lv_obj_create(tail);
    plain(lock_[p]);
    lv_obj_set_size(lock_[p], 14, 17);
    lv_obj_align(lock_[p], LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t* shackle = lv_obj_create(lock_[p]);
    plain(shackle);
    lv_obj_set_size(shackle, 8, 11);
    lv_obj_align(shackle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_border_width(shackle, 2, 0);
    lv_obj_set_style_border_color(shackle, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_radius(shackle, 4, 0);
    lv_obj_t* lockBody = lv_obj_create(lock_[p]);
    plain(lockBody);
    lv_obj_set_size(lockBody, 14, 9);
    lv_obj_align(lockBody, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(lockBody, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(lockBody, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_radius(lockBody, 2, 0);

    forget_[p] = lv_button_create(tail);
    lv_obj_set_size(forget_[p], 38, 28);
    lv_obj_align(forget_[p], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(forget_[p], lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_obj_set_user_data(forget_[p], (void*)(uintptr_t)p);
    lv_obj_add_event_cb(forget_[p], onForgetRow, LV_EVENT_CLICKED, this);
    lv_obj_t* tl = lv_label_create(forget_[p]);
    lv_label_set_text(tl, LV_SYMBOL_TRASH);
    lv_obj_center(tl);
  }

  // ---- model ----
  int findDev(const uint8_t* addr, uint8_t type) const {
    for (int i = 0; i < n_; i++)
      if (all_[i].addr_type == type && memcmp(all_[i].addr, addr, 6) == 0) return i;
    return -1;
  }

  void setMsg(const char* a, const char* b = "") {
    snprintf(msg_, sizeof msg_, "%s%s", a, b);
    msgTicks_ = kMsgTicks;
    msgSerial_++;
  }

  // Detects the end of a connection attempt started from this page.
  void checkOutcome(os::BleState st) {
    if (!pending_) return;
    const int i = findDev(pendAddr_, pendType_);
    if (i >= 0 && all_[i].connected) {
      pending_ = false;
      char nm[40];
      fmtName(all_[i], nm, sizeof nm);
      setMsg("Connected to ", nm);
    } else if (st == os::BleState::Off) {
      pending_ = false;
    } else if (st != os::BleState::Connecting) {
      pending_ = false;
      setMsg("Could not connect or pair with ", pendName_);
    }
  }

  void refresh(bool force) {
    if (!ble_) return;
    const os::BleState st = ble_->state();
    const bool en = ble_->enabled();
    n_ = en ? ble_->devices(all_, kMaxDev) : 0;
    if (autoScan_ && st == os::BleState::Idle) {
      autoScan_ = false;
      ble_->startScan(kScanMs);
    }
    checkOutcome(st);

    nconn_ = nlist_ = 0;
    for (int i = 0; i < n_; i++) {
      if (all_[i].connected) {
        if (nconn_ < kMaxConn) conn_[nconn_++] = (uint8_t)i;
      } else if (all_[i].connectable || all_[i].bonded) {
        list_idx_[nlist_++] = (uint8_t)i;
      }
    }

    // Skip the widget updates when nothing visible changed.
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
    mix((uint32_t)st);
    mix(en);
    mix(msgTicks_ > 0 ? msgSerial_ : 0);
    mix(n_);
    for (int i = 0; i < n_; i++) {
      const os::BleDevice& d = all_[i];
      for (const char* c = d.name; *c; c++) mix((uint8_t)*c);
      for (int k = 0; k < 6; k++) mix(d.addr[k]);
      mix((uint32_t)rssiBars(d) | (d.bonded << 4) | (d.connected << 5) | (d.connectable << 6) |
          ((uint32_t)(d.battery + 1) << 8));
    }
    if (!force && h == hash_) return;
    hash_ = h;

    updateTop(st, en);
    updateConn();
    updateList();
  }

  // ---- view ----
  void updateTop(os::BleState st, bool en) {
    char txt[64];
    const bool transition = en != (st != os::BleState::Off);
    if (msgTicks_ > 0) {
      snprintf(txt, sizeof txt, "%s", msg_);
    } else if (transition) {
      snprintf(txt, sizeof txt, en ? "Starting..." : "Turning off...");
    } else {
      switch (st) {
        case os::BleState::Off: snprintf(txt, sizeof txt, "Bluetooth is off"); break;
        case os::BleState::Idle: snprintf(txt, sizeof txt, "Not scanning"); break;
        case os::BleState::Scanning: snprintf(txt, sizeof txt, "Scanning..."); break;
        case os::BleState::Connecting: snprintf(txt, sizeof txt, "Connecting..."); break;
        case os::BleState::Connected:
          snprintf(txt, sizeof txt, "%d connected", nconn_);
          break;
      }
    }
    lv_label_set_text(status_, txt);

    if (en) lv_obj_add_state(sw_, LV_STATE_CHECKED);
    else lv_obj_remove_state(sw_, LV_STATE_CHECKED);
    if (transition) lv_obj_add_state(sw_, LV_STATE_DISABLED);
    else lv_obj_remove_state(sw_, LV_STATE_DISABLED);

    const bool scanning = st == os::BleState::Scanning;
    const bool can_scan = !transition && st != os::BleState::Off && st != os::BleState::Connecting;
    if (can_scan) lv_obj_remove_state(scanBtn_, LV_STATE_DISABLED);
    else lv_obj_add_state(scanBtn_, LV_STATE_DISABLED);
    lv_label_set_text(scanLbl_, scanning ? "Stop" : "Scan");

    const bool busy = scanning || st == os::BleState::Connecting;
    if (busy && !spinner_) {
      spinner_ = lv_spinner_create(spinHolder_);
      lv_spinner_set_anim_params(spinner_, 1000, 200);
      lv_obj_set_size(spinner_, 24, 24);
      lv_obj_set_style_arc_width(spinner_, 4, LV_PART_MAIN);
      lv_obj_set_style_arc_width(spinner_, 4, LV_PART_INDICATOR);
      lv_obj_center(spinner_);
    } else if (!busy && spinner_) {
      lv_obj_delete(spinner_);
      spinner_ = nullptr;
    }
  }

  void updateConn() {
    if (nconn_ == 0) {
      lv_obj_add_flag(connBox_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(connBox_, LV_OBJ_FLAG_HIDDEN);
    }
    for (int k = 0; k < kMaxConn; k++) {
      if (k >= nconn_) {
        lv_obj_add_flag(connRow_[k], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_remove_flag(connRow_[k], LV_OBJ_FLAG_HIDDEN);
      const os::BleDevice& d = all_[conn_[k]];
      char nm[40];
      fmtName(d, nm, sizeof nm);
      lv_label_set_text(connName_[k], nm);
      if (d.battery >= 0) lv_label_set_text_fmt(connBatt_[k], LV_SYMBOL_BATTERY_FULL " %d%%", d.battery);
      else lv_label_set_text(connBatt_[k], "");
      connDev_[k] = d;
    }
  }

  void updateList() {
    lv_obj_set_height(sizer_, nlist_ > 0 ? nlist_ * kRowH : 1);
    const os::BleState st = ble_->state();
    if (nlist_ == 0) {
      lv_label_set_text(empty_, st == os::BleState::Off ? "" : "No devices found. Tap Scan.");
      lv_obj_remove_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    }
    for (int p = 0; p < kPool; p++) rowItem_[p] = -2;     // force a rebind
    bindRows();
  }

  void bindRows() {
    int first = lv_obj_get_scroll_y(list_) / kRowH;
    if (first < 0) first = 0;
    for (int p = 0; p < kPool; p++) {
      // Smallest item >= first handled by this pool row.
      int item = first + ((p - first) % kPool + kPool) % kPool;
      if (item >= nlist_) {
        if (rowItem_[p] != -1) {
          lv_obj_add_flag(row_[p], LV_OBJ_FLAG_HIDDEN);
          rowItem_[p] = -1;
        }
        continue;
      }
      if (rowItem_[p] == item) continue;
      rowItem_[p] = item;
      const os::BleDevice& d = all_[list_idx_[item]];
      rowDev_[p] = d;
      char nm[40];
      fmtName(d, nm, sizeof nm);
      lv_label_set_text(rowName_[p], nm);
      const int bars = rssiBars(d);
      for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(bar_[p][i], i < bars ? colAccent() : colDim(), 0);
      if (d.bonded) {
        lv_obj_remove_flag(lock_[p], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(forget_[p], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(lock_[p], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(forget_[p], LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_set_pos(row_[p], 0, item * kRowH);
      lv_obj_remove_flag(row_[p], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // ---- actions ----
  void connectTo(const os::BleDevice& d) {
    char nm[40];
    fmtName(d, nm, sizeof nm);
    if (ble_->connect(d)) {
      pending_ = true;
      memcpy(pendAddr_, d.addr, 6);
      pendType_ = d.addr_type;
      snprintf(pendName_, sizeof pendName_, "%s", nm);
      setMsg("Connecting to ", nm);
    } else {
      setMsg("Bluetooth is busy, try again");
    }
    refresh(true);
  }

  // ---- callbacks ----
  static BluetoothPage* self(lv_event_t* e) {
    return static_cast<BluetoothPage*>(lv_event_get_user_data(e));
  }
  static int idxOf(lv_event_t* e) {
    return (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
  }

  static void onSwitch(lv_event_t* e) {
    auto* s = self(e);
    if (!s->ble_) return;
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    s->msgTicks_ = 0;
    s->autoScan_ = on;
    s->ble_->setEnabled(on);
    s->refresh(true);
  }

  static void onScan(lv_event_t* e) {
    auto* s = self(e);
    if (!s->ble_) return;
    s->msgTicks_ = 0;
    if (s->ble_->state() == os::BleState::Scanning) {
      s->ble_->stopScan();
    } else if (!s->ble_->startScan(kScanMs)) {
      s->setMsg("Cannot scan right now");
    }
    s->refresh(true);
  }

  static void onRow(lv_event_t* e) {
    auto* s = self(e);
    const int p = idxOf(e);
    if (!s->ble_ || s->rowItem_[p] < 0) return;
    s->connectTo(s->rowDev_[p]);
  }

  static void onForgetRow(lv_event_t* e) {
    auto* s = self(e);
    const int p = idxOf(e);
    if (!s->ble_ || s->rowItem_[p] < 0) return;
    s->ble_->forget(s->rowDev_[p]);
    s->refresh(true);
  }

  static void onDisconnect(lv_event_t* e) {
    auto* s = self(e);
    const int k = idxOf(e);
    if (!s->ble_ || k >= s->nconn_) return;
    s->ble_->disconnect(s->connDev_[k]);
    s->refresh(true);
  }

  static void onForgetConn(lv_event_t* e) {
    auto* s = self(e);
    const int k = idxOf(e);
    if (!s->ble_ || k >= s->nconn_) return;
    s->ble_->forget(s->connDev_[k]);
    s->refresh(true);
  }

  static void onScroll(lv_event_t* e) { self(e)->bindRows(); }

  static void onTimer(lv_timer_t* t) {
    auto* s = static_cast<BluetoothPage*>(lv_timer_get_user_data(t));
    if (s->msgTicks_ > 0 && --s->msgTicks_ == 0) s->hash_ = 0;     // message expired: redraw
    s->refresh(false);
  }

  os::Ble* ble_ = nullptr;
  lv_timer_t* timer_ = nullptr;

  lv_obj_t *status_ = nullptr, *spinHolder_ = nullptr, *spinner_ = nullptr, *scanBtn_ = nullptr,
           *scanLbl_ = nullptr, *sw_ = nullptr, *connBox_ = nullptr, *list_ = nullptr,
           *sizer_ = nullptr, *empty_ = nullptr;
  lv_obj_t *connRow_[kMaxConn], *connName_[kMaxConn], *connBatt_[kMaxConn];
  lv_obj_t *row_[kPool], *rowName_[kPool], *lock_[kPool], *forget_[kPool];
  lv_obj_t* bar_[kPool][4];
  int rowItem_[kPool];
  os::BleDevice rowDev_[kPool];
  os::BleDevice connDev_[kMaxConn];

  os::BleDevice all_[kMaxDev];
  int n_ = 0, nconn_ = 0, nlist_ = 0;
  uint8_t conn_[kMaxConn];
  uint8_t list_idx_[kMaxDev];
  uint32_t hash_ = 0;

  bool autoScan_ = false;
  bool pending_ = false;
  uint8_t pendAddr_[6] = {};
  uint8_t pendType_ = 0;
  char pendName_[40] = {};
  char msg_[80] = {};
  int msgTicks_ = 0;
  uint32_t msgSerial_ = 0;
};

Page* makeBluetoothPage(std::pmr::memory_resource& m) {
  return new (m.allocate(sizeof(BluetoothPage), alignof(BluetoothPage))) BluetoothPage();
}

}  // namespace apps::settings
