// Wi-Fi page: on/off switch, current connection, scanned networks and the password overlay.
//
// Memory notes: the network list is a virtual list. Only kPool rows exist, they are reused
// while scrolling and rebound to scan results (24 at most). The page object (rows, scan
// results, overlay state) is constructed in the app arena; LVGL objects are owned by LVGL.
#include <cstdio>
#include <cstring>
#include <string.h>

#include "page.h"

namespace apps::settings {

namespace {

constexpr int kMaxAps = 24;
constexpr int kPool = 8;          // reusable rows; covers the tallest list area plus margin
constexpr int kRowH = 36;         // row pitch (row height 34 + 2 gap)
constexpr int kMinPass = 8;       // WPA2 passphrase minimum

// WifiStatus::fail_reason values published by the Wi-Fi service.
constexpr uint8_t kFailWrongPassword = 1;
constexpr uint8_t kFailNotFound = 2;
constexpr uint8_t kFailTimeout = 3;

constexpr uint32_t kCardBg = 0x1C2128;
constexpr uint32_t kCardPressed = 0x2A323D;
constexpr uint32_t kOverlayBg = 0x0B0E12;

const char* failText(uint8_t reason) {
  switch (reason) {
    case kFailWrongPassword: return "Wrong password";
    case kFailNotFound: return "Network not found";
    case kFailTimeout: return "Connection timed out";
    default: return "Connection failed";
  }
}

int barsFor(int rssi) { return rssi >= -60 ? 3 : (rssi >= -72 ? 2 : 1); }

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

lv_obj_t* makeButton(lv_obj_t* parent, int w, int h, const char* text, lv_color_t bg,
                     lv_event_cb_t cb, void* user) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

void setButtonText(lv_obj_t* b, const char* text) { lv_label_set_text(lv_obj_get_child(b, 0), text); }

void showObj(lv_obj_t* o, bool on) {
  if (on)
    lv_obj_set_hidden(o, false);
  else
    lv_obj_set_hidden(o, true);
}

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  return o;
}

}  // namespace

class WifiPage final : public Page {
 public:
  const char* title() const override { return "Wi-Fi"; }

  void build(lv_obj_t* body, lv_obj_t* root, os::Context& ctx) override {
    net_ = ctx.net();
    root_ = root;
    body_ = body;
    const lv_color_t white = lv_color_white();
    const lv_color_t grey = lv_palette_lighten(LV_PALETTE_GREY, 1);

    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);

    if (!net_) {
      lv_obj_t* l = makeLabel(body, "Wi-Fi is not available", nullptr, white);
      lv_obj_center(l);
      return;
    }

    // --- row 0: switch, caption, refresh ---
    lv_obj_t* top = plain(body);
    lv_obj_set_size(top, lv_pct(100), 38);
    sw_ = lv_switch_create(top);
    lv_obj_set_size(sw_, 54, 28);
    lv_obj_align(sw_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(sw_, onSwitch, LV_EVENT_VALUE_CHANGED, this);
    makeLabel(top, "Wi-Fi", &lv_font_montserrat_20, white);
    lv_obj_align(lv_obj_get_child(top, -1), LV_ALIGN_LEFT_MID, 66, 0);
    refreshBtn_ = makeButton(top, 112, 34, LV_SYMBOL_REFRESH " Refresh",
                             lv_palette_darken(LV_PALETTE_BLUE_GREY, 2), onRefresh, this);
    lv_obj_align(refreshBtn_, LV_ALIGN_RIGHT_MID, 0, 0);

    // --- connection card ---
    card_ = plain(body);
    lv_obj_set_size(card_, lv_pct(100), 54);
    lv_obj_set_style_bg_color(card_, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_opa(card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card_, 8, 0);
    spinner_ = lv_spinner_create(card_);
    lv_spinner_set_anim_params(spinner_, 1000, 200);
    lv_obj_set_size(spinner_, 28, 28);
    lv_obj_set_style_arc_width(spinner_, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner_, 4, LV_PART_INDICATOR);
    lv_obj_align(spinner_, LV_ALIGN_LEFT_MID, 10, 0);
    cardL1_ = makeLabel(card_, "", &lv_font_montserrat_20, white);
    cardL2_ = makeLabel(card_, "", nullptr, grey);
    lv_label_set_long_mode(cardL1_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(cardL2_, LV_LABEL_LONG_MODE_DOTS);
    btnA_ = makeButton(card_, 100, 36, "Disconnect", lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
                       onCardA, this);
    lv_obj_align(btnA_, LV_ALIGN_RIGHT_MID, -88, 0);
    btnB_ = makeButton(card_, 76, 36, "Forget", lv_palette_darken(LV_PALETTE_RED, 2), onCardB, this);
    lv_obj_align(btnB_, LV_ALIGN_RIGHT_MID, -8, 0);
    showObj(card_, false);

    // --- virtual list ---
    list_ = lv_obj_create(body);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, lv_pct(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(list_, onListScroll, LV_EVENT_SCROLL, this);
    lv_obj_add_event_cb(list_, onListScroll, LV_EVENT_SIZE_CHANGED, this);
    inner_ = plain(list_);
    lv_obj_set_clickable(inner_, false);
    lv_obj_set_size(inner_, lv_pct(100), 0);
    lv_obj_set_pos(inner_, 0, 0);
    empty_ = makeLabel(list_, "", nullptr, grey);
    lv_obj_set_floating(empty_, true);
    lv_obj_align(empty_, LV_ALIGN_TOP_MID, 0, 24);
    for (int i = 0; i < kPool; i++) makeRow(rows_[i]);

    refresh();
    maybeScan();
  }

  void onEvent(const os::Msg& m) override {
    if (!net_ || m.topic != os::Topic::Network) return;
    refresh();
    maybeScan();
  }

  void teardown() override {
    if (closeTimer_) {
      lv_timer_delete(closeTimer_);
      closeTimer_ = nullptr;
    }
    if (ov_) {
      lv_obj_delete(ov_);
      ov_ = nullptr;
    }
    net_ = nullptr;
  }

 private:
  struct Row {
    WifiPage* page = nullptr;
    lv_obj_t* obj = nullptr;
    lv_obj_t* bar[3] = {};
    lv_obj_t* ssid = nullptr;
    lv_obj_t* tag = nullptr;
    lv_obj_t* lockBody = nullptr;
    lv_obj_t* lockArc = nullptr;
    int idx = -1;
  };

  // ---------------------------------------------------------------- list rows
  void makeRow(Row& r) {
    r.page = this;
    r.obj = plain(inner_);
    lv_obj_set_size(r.obj, lv_pct(100), kRowH - 2);
    lv_obj_set_style_bg_color(r.obj, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_color(r.obj, lv_color_hex(kCardPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r.obj, 8, 0);
    lv_obj_set_clickable(r.obj, true);
    lv_obj_add_event_cb(r.obj, onRow, LV_EVENT_CLICKED, &r);
    for (int i = 0; i < 3; i++) {
      r.bar[i] = plain(r.obj);
      lv_obj_set_size(r.bar[i], 4, 6 + 4 * i);
      lv_obj_set_style_radius(r.bar[i], 1, 0);
      lv_obj_set_style_bg_opa(r.bar[i], LV_OPA_COVER, 0);
      lv_obj_align(r.bar[i], LV_ALIGN_BOTTOM_LEFT, 12 + 7 * i, -9);
    }
    r.ssid = makeLabel(r.obj, "", &lv_font_montserrat_20, lv_color_white());
    lv_label_set_long_mode(r.ssid, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(r.ssid, 270);
    lv_obj_align(r.ssid, LV_ALIGN_LEFT_MID, 44, 0);
    r.tag = makeLabel(r.obj, "", nullptr, lv_palette_main(LV_PALETTE_LIGHT_BLUE));
    lv_obj_align(r.tag, LV_ALIGN_RIGHT_MID, -36, 0);
    r.lockArc = plain(r.obj);                       // shackle
    lv_obj_set_size(r.lockArc, 8, 8);
    lv_obj_set_style_radius(r.lockArc, 4, 0);
    lv_obj_set_style_border_width(r.lockArc, 2, 0);
    lv_obj_set_style_border_color(r.lockArc, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(r.lockArc, LV_ALIGN_RIGHT_MID, -13, -5);
    r.lockBody = plain(r.obj);
    lv_obj_set_size(r.lockBody, 12, 9);
    lv_obj_set_style_radius(r.lockBody, 2, 0);
    lv_obj_set_style_bg_color(r.lockBody, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_style_bg_opa(r.lockBody, LV_OPA_COVER, 0);
    lv_obj_align(r.lockBody, LV_ALIGN_RIGHT_MID, -11, 3);
    lv_obj_t* kids[] = {r.bar[0], r.bar[1], r.bar[2], r.lockArc, r.lockBody};
    for (lv_obj_t* k : kids) lv_obj_set_clickable(k, false);
    showObj(r.obj, false);
  }

  void bindRow(Row& r, int idx) {
    const os::WifiAp& ap = aps_[idx];
    r.idx = idx;
    lv_obj_set_y(r.obj, idx * kRowH);
    lv_label_set_text(r.ssid, ap.ssid);
    const int b = barsFor(ap.rssi);
    for (int i = 0; i < 3; i++)
      lv_obj_set_style_bg_color(r.bar[i], i < b ? lv_color_hex(0x5BC0EB) : lv_color_hex(0x3A424D), 0);
    showObj(r.lockBody, ap.secured);
    showObj(r.lockArc, ap.secured);
    const bool cur = std::strcmp(st_.ssid, ap.ssid) == 0;
    if (cur && st_.state == os::WifiState::Connected) {
      lv_label_set_text(r.tag, "connected");
      lv_obj_set_style_text_color(r.tag, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else if (cur && st_.state == os::WifiState::Connecting) {
      lv_label_set_text(r.tag, "connecting");
      lv_obj_set_style_text_color(r.tag, lv_palette_main(LV_PALETTE_AMBER), 0);
    } else if (ap.saved) {
      lv_label_set_text(r.tag, "saved");
      lv_obj_set_style_text_color(r.tag, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    } else {
      lv_label_set_text(r.tag, "");
    }
  }

  // Binds the rows that intersect the viewport; the slot of an item is idx % kPool.
  void relayout() {
    if (!list_ || !inner_) return;
    const int top = lv_obj_get_scroll_y(list_);
    const int h = lv_obj_get_height(list_);
    int first = top / kRowH;
    int last = (top + h) / kRowH;
    if (last >= n_) last = n_ - 1;
    if (last > first + kPool - 1) last = first + kPool - 1;
    bool used[kPool] = {};
    for (int idx = first; idx <= last; idx++) {
      const int slot = idx % kPool;
      Row& r = rows_[slot];
      if (r.idx != idx || dirty_) bindRow(r, idx);
      showObj(r.obj, true);
      used[slot] = true;
    }
    for (int s = 0; s < kPool; s++) {
      if (!used[s]) {
        rows_[s].idx = -1;
        showObj(rows_[s].obj, false);
      }
    }
    dirty_ = false;
  }

  // ---------------------------------------------------------------- refresh
  void refresh() {
    net_->status(st_);
    n_ = net_->scanResults(aps_, kMaxAps);
    const bool on = net_->enabled();
    if (on != lv_obj_has_state(sw_, LV_STATE_CHECKED)) {
      if (on)
        lv_obj_add_state(sw_, LV_STATE_CHECKED);
      else
        lv_obj_remove_state(sw_, LV_STATE_CHECKED);
    }
    const bool ready = on && st_.state != os::WifiState::Off;
    const bool scanning = st_.state == os::WifiState::Scanning;
    if (ready && !scanning && st_.state != os::WifiState::Connecting)
      lv_obj_remove_state(refreshBtn_, LV_STATE_DISABLED);
    else
      lv_obj_add_state(refreshBtn_, LV_STATE_DISABLED);
    setButtonText(refreshBtn_, scanning ? "Scanning..." : LV_SYMBOL_REFRESH " Refresh");

    updateCard(ready);

    dirty_ = true;
    lv_obj_set_height(inner_, n_ * kRowH);
    if (!on)
      lv_label_set_text(empty_, "Wi-Fi is off");
    else if (!ready)
      lv_label_set_text(empty_, "Turning on...");
    else if (scanning)
      lv_label_set_text(empty_, "Scanning...");
    else
      lv_label_set_text(empty_, "No networks found");
    showObj(empty_, n_ == 0);
    relayout();
    updateOverlay();
  }

  void updateCard(bool ready) {
    char buf[64];
    const os::WifiState s = st_.state;
    const bool vis = ready && (s == os::WifiState::Connecting || s == os::WifiState::Connected ||
                               s == os::WifiState::Failed);
    showObj(card_, vis);
    if (!vis) return;
    const bool busy = s == os::WifiState::Connecting;
    showObj(spinner_, busy);
    const int x = busy ? 50 : 12;
    lv_obj_set_width(cardL1_, busy ? 205 : 243);
    lv_obj_set_width(cardL2_, busy ? 205 : 243);
    lv_obj_align(cardL1_, LV_ALIGN_TOP_LEFT, x, 3);
    lv_obj_align(cardL2_, LV_ALIGN_TOP_LEFT, x, 30);
    lv_obj_set_style_text_color(cardL2_, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    if (busy) {
      lv_label_set_text(cardL1_, "Connecting...");
      lv_label_set_text(cardL2_, st_.ssid[0] ? st_.ssid : "Looking for saved networks");
      setButtonText(btnA_, "Cancel");
      showObj(btnB_, false);
    } else if (s == os::WifiState::Connected) {
      lv_label_set_text(cardL1_, st_.ssid);
      const uint32_t ip = st_.ip;
      std::snprintf(buf, sizeof buf, "%u.%u.%u.%u   %d dBm", (unsigned)(ip >> 24),
                    (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255),
                    (int)st_.rssi);
      lv_label_set_text(cardL2_, buf);
      setButtonText(btnA_, "Disconnect");
      showObj(btnB_, true);
    } else {
      lv_label_set_text(cardL1_, st_.ssid[0] ? st_.ssid : "Wi-Fi");
      lv_label_set_text(cardL2_, failText(st_.fail_reason));
      lv_obj_set_style_text_color(cardL2_, lv_palette_main(LV_PALETTE_RED), 0);
      setButtonText(btnA_, "Dismiss");
      showObj(btnB_, false);
    }
    lv_obj_align(btnA_, LV_ALIGN_RIGHT_MID, busy || s == os::WifiState::Failed ? -8 : -88, 0);
  }

  void maybeScan() {
    if (scanned_ || !net_->enabled()) return;
    const os::WifiState s = st_.state;
    if (s == os::WifiState::Scanning) {
      scanned_ = true;
      return;
    }
    if (s == os::WifiState::Idle || s == os::WifiState::Connected || s == os::WifiState::Failed) {
      if (net_->scan()) scanned_ = true;
    }
  }

  // ---------------------------------------------------------------- callbacks
  static WifiPage* page(lv_event_t* e) { return static_cast<WifiPage*>(lv_event_get_user_data(e)); }

  static void onSwitch(lv_event_t* e) {
    WifiPage* p = page(e);
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    p->scanned_ = false;
    p->net_->setEnabled(on);
    p->n_ = 0;
    p->refresh();
  }
  static void onRefresh(lv_event_t* e) {
    WifiPage* p = page(e);
    if (p->net_->scan()) p->scanned_ = true;
    p->refresh();
  }
  static void onCardA(lv_event_t* e) {
    WifiPage* p = page(e);
    p->net_->disconnect();
  }
  static void onCardB(lv_event_t* e) {
    WifiPage* p = page(e);
    char ssid[33];
    strlcpy(ssid, p->st_.ssid, sizeof ssid);
    p->net_->forget(ssid);
  }
  static void onListScroll(lv_event_t* e) { page(e)->relayout(); }

  static void onRow(lv_event_t* e) {
    Row* r = static_cast<Row*>(lv_event_get_user_data(e));
    WifiPage* p = r->page;
    if (!p->net_ || r->idx < 0 || r->idx >= p->n_) return;
    const os::WifiAp ap = p->aps_[r->idx];
    if (std::strcmp(ap.ssid, p->st_.ssid) == 0 &&
        (p->st_.state == os::WifiState::Connected || p->st_.state == os::WifiState::Connecting))
      return;
    if (ap.saved) {
      p->net_->connect(ap.ssid, nullptr, true);
    } else if (!ap.secured) {
      p->net_->connect(ap.ssid, "", true);
    } else {
      p->openOverlay(ap.ssid);
    }
  }

  // ---------------------------------------------------------------- password overlay
  void createOverlay() {
    const int W = lv_obj_get_content_width(root_);
    const int H = lv_obj_get_content_height(root_);
    const lv_color_t white = lv_color_white();
    ov_ = plain(root_);
    lv_obj_set_floating(ov_, true);
    lv_obj_set_size(ov_, W, H);
    lv_obj_set_pos(ov_, 0, 0);
    lv_obj_set_style_bg_color(ov_, lv_color_hex(kOverlayBg), 0);
    lv_obj_set_style_bg_opa(ov_, LV_OPA_COVER, 0);

    // --- form: cancel / SSID / connect, password row, keyboard ---
    form_ = plain(ov_);
    lv_obj_set_size(form_, W, H);
    lv_obj_set_pos(form_, 0, 0);
    lv_obj_t* cancel = makeButton(form_, 92, 36, "Cancel", lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
                                  onOvCancel, this);
    lv_obj_set_pos(cancel, 0, 0);
    ovTitle_ = makeLabel(form_, "", &lv_font_montserrat_20, white);
    lv_label_set_long_mode(ovTitle_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(ovTitle_, W - 92 - 104 - 16);
    lv_obj_set_style_text_align(ovTitle_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ovTitle_, 100, 6);
    connectBtn_ = makeButton(form_, 100, 36, "Connect", lv_palette_darken(LV_PALETTE_GREEN, 2),
                             onOvConnect, this);
    lv_obj_set_pos(connectBtn_, W - 100, 0);

    ta_ = lv_textarea_create(form_);
    lv_textarea_set_one_line(ta_, true);
    lv_textarea_set_max_length(ta_, 63);
    lv_textarea_set_placeholder_text(ta_, "Password (min 8 characters)");
    lv_textarea_set_password_mode(ta_, true);
    lv_obj_set_style_text_font(ta_, &lv_font_montserrat_20, 0);
    lv_obj_set_size(ta_, W - 48, 44);
    lv_obj_set_pos(ta_, 0, 42);
    // The default padding leaves less height than one line of the 20 px font, which made the
    // text scroll vertically. Keep the box tall enough and allow horizontal scrolling only.
    lv_obj_set_style_pad_ver(ta_, 9, 0);
    lv_obj_set_scrollbar_mode(ta_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(ta_, LV_DIR_HOR);
    lv_obj_add_state(ta_, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ta_, onOvText, LV_EVENT_VALUE_CHANGED, this);
    eyeBtn_ = makeButton(form_, 44, 44, LV_SYMBOL_EYE_CLOSE, lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
                         onOvEye, this);
    lv_obj_set_pos(eyeBtn_, W - 44, 42);

    kb_ = lv_keyboard_create(form_);
    // lv_keyboard_create() aligns the keyboard to the bottom; set_pos would add to that offset
    // and push it off the area (only two rows were visible). Align to the top-left instead.
    lv_obj_set_size(kb_, W, H - 92);
    lv_obj_align(kb_, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_keyboard_set_mode(kb_, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb_, ta_);
    lv_obj_add_event_cb(kb_, onOvKb, LV_EVENT_READY, this);
    lv_obj_add_event_cb(kb_, onOvKb, LV_EVENT_CANCEL, this);

    // --- progress / result view ---
    prog_ = plain(ov_);
    lv_obj_set_size(prog_, W, H);
    lv_obj_set_pos(prog_, 0, 0);
    progSpinner_ = lv_spinner_create(prog_);
    lv_spinner_set_anim_params(progSpinner_, 1000, 200);
    lv_obj_set_size(progSpinner_, 56, 56);
    lv_obj_set_pos(progSpinner_, (W - 56) / 2, 36);
    progMsg_ = makeLabel(prog_, "", &lv_font_montserrat_28, white);
    lv_obj_set_width(progMsg_, W);
    lv_obj_set_style_text_align(progMsg_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(progMsg_, 0, 108);
    progDetail_ = makeLabel(prog_, "", &lv_font_montserrat_20, lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_label_set_long_mode(progDetail_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(progDetail_, W - 40);
    lv_obj_set_style_text_align(progDetail_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(progDetail_, 20, 154);
    retryBtn_ = makeButton(prog_, 130, 42, "Retry", lv_palette_darken(LV_PALETTE_GREEN, 2), onOvRetry, this);
    lv_obj_set_pos(retryBtn_, W / 2 - 140, H - 56);
    closeBtn_ = makeButton(prog_, 130, 42, "Close", lv_palette_darken(LV_PALETTE_BLUE_GREY, 2),
                           onOvClose, this);
    lv_obj_set_pos(closeBtn_, W / 2 + 10, H - 56);
  }

  void openOverlay(const char* ssid) {
    if (!ov_) createOverlay();
    strlcpy(target_, ssid, sizeof target_);
    lv_label_set_text(ovTitle_, target_);
    lv_textarea_set_text(ta_, "");
    lv_textarea_set_password_mode(ta_, true);
    setButtonText(eyeBtn_, LV_SYMBOL_EYE_CLOSE);
    lv_keyboard_set_mode(kb_, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_state(connectBtn_, LV_STATE_DISABLED);
    busy_ = false;
    showForm(true);
    showObj(ov_, true);
    lv_obj_move_foreground(ov_);
  }

  void closeOverlay() {
    if (closeTimer_) {
      lv_timer_delete(closeTimer_);
      closeTimer_ = nullptr;
    }
    busy_ = false;
    if (!ov_) return;
    lv_textarea_set_text(ta_, "");                  // do not keep the password around
    showObj(ov_, false);
  }

  void showForm(bool form) {
    showObj(form_, form);
    showObj(prog_, !form);
  }

  void startConnect() {
    const char* pw = lv_textarea_get_text(ta_);
    if (std::strlen(pw) < (size_t)kMinPass) return;
    net_->connect(target_, pw, true);
    lv_textarea_set_text(ta_, "");
    busy_ = true;
    showProgress(os::WifiState::Connecting);
  }

  void showProgress(os::WifiState s) {
    showForm(false);
    const bool conn = s == os::WifiState::Connecting;
    const bool ok = s == os::WifiState::Connected;
    showObj(progSpinner_, conn);
    showObj(retryBtn_, !conn && !ok);
    lv_obj_set_style_text_color(progMsg_, ok ? lv_palette_main(LV_PALETTE_GREEN)
                                             : (conn ? lv_color_white() : lv_palette_main(LV_PALETTE_RED)), 0);
    if (conn) {
      lv_label_set_text(progMsg_, "Connecting...");
      lv_label_set_text(progDetail_, target_);
      setButtonText(closeBtn_, "Cancel");
    } else if (ok) {
      char buf[48];
      const uint32_t ip = st_.ip;
      std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255),
                    (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
      lv_label_set_text(progMsg_, "Connected");
      lv_label_set_text(progDetail_, buf);
      setButtonText(closeBtn_, "Done");
    } else {
      lv_label_set_text(progMsg_, failText(st_.fail_reason));
      lv_label_set_text(progDetail_, target_);
      setButtonText(closeBtn_, "Close");
    }
    // Success: a single centred button.
    lv_obj_set_x(closeBtn_, ok || conn ? (lv_obj_get_width(prog_) - 130) / 2 : lv_obj_get_width(prog_) / 2 + 10);
  }

  // Follows the connection attempt started from the overlay.
  void updateOverlay() {
    if (!ov_ || !busy_ || lv_obj_is_hidden(ov_)) return;
    switch (st_.state) {
      case os::WifiState::Connecting:
        showProgress(os::WifiState::Connecting);
        break;
      case os::WifiState::Connected:
        if (std::strcmp(st_.ssid, target_) != 0) break;
        showProgress(os::WifiState::Connected);
        busy_ = false;
        if (!closeTimer_) {
          closeTimer_ = lv_timer_create(onCloseTimer, 1500, this);
          lv_timer_set_repeat_count(closeTimer_, 1);
        }
        break;
      case os::WifiState::Failed:
        showProgress(os::WifiState::Failed);
        busy_ = false;
        break;
      case os::WifiState::Off:
        closeOverlay();
        break;
      default:
        break;
    }
  }

  static void onCloseTimer(lv_timer_t* t) {
    WifiPage* p = static_cast<WifiPage*>(lv_timer_get_user_data(t));
    p->closeTimer_ = nullptr;                       // the timer deletes itself after this call
    p->closeOverlay();
  }

  static void onOvCancel(lv_event_t* e) { page(e)->closeOverlay(); }
  static void onOvConnect(lv_event_t* e) { page(e)->startConnect(); }
  static void onOvKb(lv_event_t* e) {
    WifiPage* p = page(e);
    if (lv_event_get_code(e) == LV_EVENT_READY)
      p->startConnect();
    else
      p->closeOverlay();
  }
  static void onOvText(lv_event_t* e) {
    WifiPage* p = page(e);
    const bool ok = std::strlen(lv_textarea_get_text(p->ta_)) >= (size_t)kMinPass;
    if (ok)
      lv_obj_remove_state(p->connectBtn_, LV_STATE_DISABLED);
    else
      lv_obj_add_state(p->connectBtn_, LV_STATE_DISABLED);
  }
  static void onOvEye(lv_event_t* e) {
    WifiPage* p = page(e);
    const bool hidden = !lv_textarea_get_password_mode(p->ta_);
    lv_textarea_set_password_mode(p->ta_, hidden);
    setButtonText(p->eyeBtn_, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
  }
  static void onOvRetry(lv_event_t* e) {
    WifiPage* p = page(e);
    p->net_->disconnect();                          // clears the failed state
    p->busy_ = false;
    p->showForm(true);
  }
  static void onOvClose(lv_event_t* e) {
    WifiPage* p = page(e);
    if (p->busy_) p->net_->disconnect();            // Cancel while connecting
    p->closeOverlay();
  }

  os::Net* net_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* body_ = nullptr;
  lv_obj_t *sw_ = nullptr, *refreshBtn_ = nullptr, *card_ = nullptr, *spinner_ = nullptr;
  lv_obj_t *cardL1_ = nullptr, *cardL2_ = nullptr, *btnA_ = nullptr, *btnB_ = nullptr;
  lv_obj_t *list_ = nullptr, *inner_ = nullptr, *empty_ = nullptr;
  Row rows_[kPool];
  os::WifiAp aps_[kMaxAps] = {};
  int n_ = 0;
  os::WifiStatus st_ = {};
  bool scanned_ = false, dirty_ = true;

  lv_obj_t *ov_ = nullptr, *form_ = nullptr, *prog_ = nullptr, *ovTitle_ = nullptr, *ta_ = nullptr;
  lv_obj_t *kb_ = nullptr, *eyeBtn_ = nullptr, *connectBtn_ = nullptr, *progSpinner_ = nullptr;
  lv_obj_t *progMsg_ = nullptr, *progDetail_ = nullptr, *retryBtn_ = nullptr, *closeBtn_ = nullptr;
  lv_timer_t* closeTimer_ = nullptr;
  char target_[33] = {};
  bool busy_ = false;
};

Page* makeWifiPage(std::pmr::memory_resource& m) {
  return new (m.allocate(sizeof(WifiPage), alignof(WifiPage))) WifiPage();
}

}  // namespace apps::settings
