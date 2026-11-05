// Wi-Fi station service. See svc_net.h.
//
// Threading model
//  * API calls (any task) only change a few flags under `g.mu` and post a command to the worker.
//  * The worker task owns every blocking esp_wifi / esp_netif / NVS call. It exists only while
//    Wi-Fi is on (plus a short grace period) so its stack is not permanently reserved.
//  * esp_event handlers and esp_timer callbacks only update the state machine and post
//    commands; they never touch NVS or build configs, so the small event-task stack is enough.
//  * `g.mu` is never held across a blocking esp_wifi call, and the user callback is invoked
//    with no lock held.
#include "svc_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

const char* const TAG = "svc_net";
constexpr int kMaxSaved = 8;
constexpr int kMaxScan = 24;
constexpr int kFetch = 40;                       // raw scan records fetched before dedup
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kDiscWatchdogMs = 3000;
constexpr uint32_t kRssiPeriodMs = 5000;
constexpr const char* kNs = "svc_net";           // NVS namespace in the default `nvs` partition

struct Saved {
  char ssid[33];
  char pass[65];
  bool used;
};

struct Big {                                     // PSRAM
  Saved saved[kMaxSaved];
  svc_net_ap_t res[kMaxScan];
  int nres;
};

enum class CmdType : uint8_t { Enable, Disable, Scan, Connect, Disconnect, Forget, Save, Auto };

struct Cmd {
  CmdType type;
  bool remember;       // Connect: store credentials once connected
  bool use_saved;      // Connect: take the password from the saved list
  bool automatic;      // Connect: started by the auto-connect logic
  char ssid[33];
  char pass[65];
};

struct G {
  SemaphoreHandle_t mu = nullptr;
  SemaphoreHandle_t qmu = nullptr;
  QueueHandle_t q = nullptr;
  bool worker_alive = false;
  svc_net_cb_t cb = nullptr;
  Big* big = nullptr;

  bool want_on = false;          // persisted user choice
  bool autoconnect = true;
  volatile bool active = false;  // stack created, event handlers registered
  bool ready = false;            // STA started
  bool link = false;             // associated and has an IP
  bool attempt = false;          // esp_wifi_connect in flight
  bool cur_auto = false;
  bool scanning = false, user_scan = false, auto_scan = false;
  bool user_disc = false;        // a disconnect we asked for is in flight
  bool pend = false;             // connect to start when that disconnect completes
  Cmd pend_cmd{};
  bool hold = false;             // suppress auto-connect (user disconnected / wrong password)
  bool remember_pending = false;
  int retry_n = 0;

  svc_net_state_t state = SVC_NET_OFF;
  uint8_t fail = 0;
  char ssid[33] = {};
  char cur_pass[65] = {};
  int8_t rssi = 0;
  uint32_t ip = 0;
  int last_bars = 0;

  esp_netif_t* netif = nullptr;
  bool loop_ours = false;
  esp_event_handler_instance_t h_wifi = nullptr, h_ip = nullptr;
  esp_timer_handle_t t_conn = nullptr, t_retry = nullptr, t_rssi = nullptr;
};

G g;

struct Lock {
  Lock() { xSemaphoreTake(g.mu, portMAX_DELAY); }
  ~Lock() { release(); }
  void release() {
    if (held_) {
      held_ = false;
      xSemaphoreGive(g.mu);
    }
  }
  bool held_ = true;
};

void notify(bool scan_done) {
  if (g.cb) g.cb(scan_done);
}

int barsFor(int rssi) { return rssi >= -60 ? 3 : (rssi >= -72 ? 2 : 1); }

bool setState(svc_net_state_t s, uint8_t fail) {            // g.mu held
  const bool ch = g.state != s || g.fail != fail;
  g.state = s;
  g.fail = fail;
  return ch;
}

int nSaved() {                                               // g.mu held
  int n = 0;
  for (auto& s : g.big->saved) n += s.used;
  return n;
}

Saved* findSaved(const char* ssid) {                         // g.mu held
  for (auto& s : g.big->saved)
    if (s.used && strcmp(s.ssid, ssid) == 0) return &s;
  return nullptr;
}

// ---- command queue -------------------------------------------------------------------------
void workerMain(void*);

bool post(const Cmd& c) {
  xSemaphoreTake(g.qmu, portMAX_DELAY);
  bool ok = true;
  if (!g.worker_alive) {
    if (xTaskCreate(workerMain, "net", 5120, nullptr, 5, nullptr) == pdPASS)
      g.worker_alive = true;
    else
      ok = false;
  }
  if (ok) ok = xQueueSend(g.q, &c, 0) == pdTRUE;
  xSemaphoreGive(g.qmu);
  if (!ok) ESP_LOGW(TAG, "command %d dropped", (int)c.type);
  return ok;
}

bool post(CmdType t) {
  Cmd c{};
  c.type = t;
  return post(c);
}

// ---- NVS -----------------------------------------------------------------------------------
void nvsLoad() {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
  uint32_t v;
  if (nvs_get_u32(h, "en", &v) == ESP_OK) g.want_on = v != 0;
  if (nvs_get_u32(h, "auto", &v) == ESP_OK) g.autoconnect = v != 0;
  for (int i = 0; i < kMaxSaved; i++) {
    char ks[4], kp[4];
    snprintf(ks, sizeof ks, "s%d", i);
    snprintf(kp, sizeof kp, "p%d", i);
    Saved& s = g.big->saved[i];
    size_t n = sizeof s.ssid;
    if (nvs_get_str(h, ks, s.ssid, &n) != ESP_OK || !s.ssid[0]) continue;
    n = sizeof s.pass;
    if (nvs_get_str(h, kp, s.pass, &n) != ESP_OK) s.pass[0] = 0;
    s.used = true;
  }
  nvs_close(h);
}

void nvsSetFlag(const char* key, bool v) {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, key, v ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
}

// Rewrites the whole saved list (rare: only on save / forget). Worker task only.
void nvsPersistSaved() {
  Saved copy[kMaxSaved];
  {
    Lock l;
    memcpy(copy, g.big->saved, sizeof copy);
  }
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
  for (int i = 0; i < kMaxSaved; i++) {
    char ks[4], kp[4];
    snprintf(ks, sizeof ks, "s%d", i);
    snprintf(kp, sizeof kp, "p%d", i);
    if (copy[i].used) {
      nvs_set_str(h, ks, copy[i].ssid);
      nvs_set_str(h, kp, copy[i].pass);
    } else {
      nvs_erase_key(h, ks);
      nvs_erase_key(h, kp);
    }
  }
  nvs_commit(h);
  nvs_close(h);
  memset(copy, 0, sizeof copy);
}

// Adds or updates a network in the RAM list. Evicts the oldest entry when full.
void savedPut(const char* ssid, const char* pass) {          // g.mu held
  Saved* s = findSaved(ssid);
  if (!s) {
    for (auto& e : g.big->saved)
      if (!e.used) {
        s = &e;
        break;
      }
  }
  if (!s) {                                                   // full: drop slot 0, shift down
    memmove(&g.big->saved[0], &g.big->saved[1], sizeof(Saved) * (kMaxSaved - 1));
    s = &g.big->saved[kMaxSaved - 1];
  }
  memset(s, 0, sizeof *s);
  strlcpy(s->ssid, ssid, sizeof s->ssid);
  strlcpy(s->pass, pass, sizeof s->pass);
  s->used = true;
}

// ---- state machine pieces ------------------------------------------------------------------
uint8_t classify(int reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
      return SVC_NET_FAIL_WRONG_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return SVC_NET_FAIL_NOT_FOUND;
    default:
      return SVC_NET_FAIL_OTHER;
  }
}

void scheduleRetry() {                                       // g.mu held
  if (!g.active || !g.autoconnect || g.hold || nSaved() == 0) return;
  const int sh = g.retry_n < 4 ? g.retry_n : 4;
  uint32_t ms = 2000u << sh;                                  // 2, 4, 8, 16, 32 -> capped
  if (ms > 30000) ms = 30000;
  g.retry_n++;
  esp_timer_stop(g.t_retry);
  esp_timer_start_once(g.t_retry, (uint64_t)ms * 1000);
}

// Completes a disconnect we requested: starts the pending connect or goes idle.
// Returns true when the state changed. g.mu held; may post a command.
bool finishUserDisc() {
  g.user_disc = false;
  esp_timer_stop(g.t_conn);
  if (g.pend) {
    g.pend = false;
    Cmd c = g.pend_cmd;
    post(c);
    return false;
  }
  if (g.state == SVC_NET_FAILED) return false;
  const bool ch = g.state != SVC_NET_IDLE || g.ssid[0];
  g.state = SVC_NET_IDLE;
  g.fail = 0;
  g.ssid[0] = 0;
  return ch;
}

void onWifiEvent(void*, esp_event_base_t, int32_t id, void* data) {
  switch (id) {
    case WIFI_EVENT_STA_START: {
      {
        Lock l;
        if (!g.active) return;
        g.ready = true;
      }
      post(CmdType::Auto);
      break;
    }
    case WIFI_EVENT_SCAN_DONE: {
      const auto* ev = static_cast<const wifi_event_sta_scan_done_t*>(data);
      const bool ok = ev->status == 0;
      uint16_t n = 0;
      wifi_ap_record_t* recs = nullptr;
      if (ok) {
        esp_wifi_scan_get_ap_num(&n);
        if (n > kFetch) n = kFetch;
        if (n) recs = static_cast<wifi_ap_record_t*>(malloc(n * sizeof(wifi_ap_record_t)));
        if (recs) {
          if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;
        } else {
          n = 0;
          esp_wifi_clear_ap_list();
        }
      } else {
        esp_wifi_clear_ap_list();
      }
      Cmd pick{};
      bool do_pick = false, state_ch = false;
      {
        Lock l;
        if (!g.active) {
          free(recs);
          return;
        }
        const bool was_auto = g.auto_scan;
        g.scanning = g.auto_scan = g.user_scan = false;
        if (ok) {
          Big& b = *g.big;
          b.nres = 0;
          for (uint16_t i = 0; i < n; i++) {
            const char* s = reinterpret_cast<const char*>(recs[i].ssid);
            if (!s[0]) continue;                                    // hidden
            int at = -1;
            for (int k = 0; k < b.nres; k++)
              if (strcmp(b.res[k].ssid, s) == 0) {
                at = k;
                break;
              }
            if (at < 0) {
              if (b.nres < kMaxScan) {
                at = b.nres++;
              } else {
                int w = 0;                                          // weakest slot
                for (int k = 1; k < b.nres; k++)
                  if (b.res[k].rssi < b.res[w].rssi) w = k;
                if (recs[i].rssi <= b.res[w].rssi) continue;
                at = w;
              }
            } else if (recs[i].rssi <= b.res[at].rssi) {
              continue;
            }
            strlcpy(b.res[at].ssid, s, sizeof b.res[at].ssid);
            b.res[at].rssi = recs[i].rssi;
            b.res[at].secured = recs[i].authmode != WIFI_AUTH_OPEN;
            b.res[at].saved = false;
          }
          for (int a = 1; a < b.nres; a++) {                        // insertion sort, strongest first
            svc_net_ap_t t = b.res[a];
            int k = a - 1;
            while (k >= 0 && b.res[k].rssi < t.rssi) {
              b.res[k + 1] = b.res[k];
              k--;
            }
            b.res[k + 1] = t;
          }
          if (was_auto && !g.hold && !g.link && !g.attempt && !g.user_disc) {
            for (int k = 0; k < b.nres && !do_pick; k++) {
              if (Saved* s = findSaved(b.res[k].ssid)) {
                pick.type = CmdType::Connect;
                pick.automatic = true;
                pick.use_saved = false;
                strlcpy(pick.ssid, s->ssid, sizeof pick.ssid);
                strlcpy(pick.pass, s->pass, sizeof pick.pass);
                do_pick = true;
              }
            }
          }
        }
        if (was_auto && !do_pick && !g.attempt && !g.link) {         // nothing to join
          if (g.state == SVC_NET_CONNECTING && !g.ssid[0]) state_ch = setState(SVC_NET_IDLE, 0);
          scheduleRetry();
        }
      }
      free(recs);
      if (do_pick) {
        post(pick);
        memset(&pick, 0, sizeof pick);
      }
      if (ok || state_ch) notify(true);
      break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
      const auto* ev = static_cast<const wifi_event_sta_disconnected_t*>(data);
      bool ch = false;
      {
        Lock l;
        if (!g.active) return;
        esp_timer_stop(g.t_rssi);
        const bool was_link = g.link;
        const bool was_attempt = g.attempt;
        g.link = false;
        g.attempt = false;
        g.ip = 0;
        if (g.user_disc) {
          ch = finishUserDisc();
        } else if (g.state == SVC_NET_CONNECTING || g.state == SVC_NET_CONNECTED || was_link) {
          esp_timer_stop(g.t_conn);
          const uint8_t cls = classify(ev->reason);
          if (cls == SVC_NET_FAIL_WRONG_PASSWORD && !was_link) {
            ch = setState(SVC_NET_FAILED, cls);
            g.remember_pending = false;
            if (g.cur_auto) g.hold = true;
          } else if (was_link) {                                    // lost an established link
            g.state = SVC_NET_IDLE;
            g.fail = 0;
            g.ssid[0] = 0;
            g.retry_n = 0;
            ch = true;
            scheduleRetry();
          } else if (was_attempt) {
            ch = setState(SVC_NET_FAILED, cls == SVC_NET_FAIL_NOT_FOUND ? cls : SVC_NET_FAIL_OTHER);
            g.remember_pending = false;
            if (g.cur_auto) scheduleRetry();
          }
        }
      }
      if (ch) notify(false);
      break;
    }
    default:
      break;
  }
}

void onIpEvent(void*, esp_event_base_t, int32_t id, void* data) {
  if (id != IP_EVENT_STA_GOT_IP) return;
  const auto* ev = static_cast<const ip_event_got_ip_t*>(data);
  Cmd save{};
  bool do_save = false;
  {
    Lock l;
    if (!g.active) return;
    esp_timer_stop(g.t_conn);
    g.attempt = false;
    g.link = true;
    g.ip = __builtin_bswap32(ev->ip_info.ip.addr);
    g.retry_n = 0;
    g.state = SVC_NET_CONNECTED;
    g.fail = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      g.rssi = ap.rssi;
      strlcpy(g.ssid, reinterpret_cast<const char*>(ap.ssid), sizeof g.ssid);
    }
    g.last_bars = barsFor(g.rssi);
    esp_timer_start_periodic(g.t_rssi, (uint64_t)kRssiPeriodMs * 1000);
    if (g.remember_pending) {
      save.type = CmdType::Save;
      strlcpy(save.ssid, g.ssid, sizeof save.ssid);
      strlcpy(save.pass, g.cur_pass, sizeof save.pass);
      do_save = true;
      g.remember_pending = false;
    }
    memset(g.cur_pass, 0, sizeof g.cur_pass);
  }
  if (do_save) {
    post(save);
    memset(&save, 0, sizeof save);
  }
  notify(false);
}

void onConnTimer(void*) {
  bool ch = false;
  {
    Lock l;
    if (!g.active) return;
    if (g.user_disc) {                                          // the disconnect event never came
      ch = finishUserDisc();
    } else if (g.state == SVC_NET_CONNECTING && g.attempt) {
      g.attempt = false;
      g.remember_pending = false;
      ch = setState(SVC_NET_FAILED, SVC_NET_FAIL_TIMEOUT);
      g.user_disc = true;                                       // swallow the resulting event
      l.release();
      if (esp_wifi_disconnect() != ESP_OK) {
        Lock l2;
        g.user_disc = false;
      } else {
        esp_timer_start_once(g.t_conn, (uint64_t)kDiscWatchdogMs * 1000);
      }
    }
  }
  if (ch) notify(false);
}

void onRetryTimer(void*) { post(CmdType::Auto); }

void onRssiTimer(void*) {
  bool ch = false;
  {
    Lock l;
    if (!g.active || g.state != SVC_NET_CONNECTED) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    g.rssi = ap.rssi;
    const int b = barsFor(ap.rssi);
    ch = b != g.last_bars;
    g.last_bars = b;
  }
  if (ch) notify(false);
}

// ---- worker --------------------------------------------------------------------------------
wifi_scan_config_t scanCfg() {
  wifi_scan_config_t sc = {};
  sc.show_hidden = false;
  sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  return sc;
}

void teardownStack() {
  {
    Lock l;
    g.active = false;
    g.ready = false;
    g.link = g.attempt = g.scanning = g.user_scan = g.auto_scan = false;
    g.user_disc = g.pend = g.remember_pending = false;
    g.state = SVC_NET_OFF;
    g.fail = 0;
    g.ssid[0] = 0;
    g.ip = 0;
    g.big->nres = 0;
    memset(g.cur_pass, 0, sizeof g.cur_pass);
  }
  esp_timer_stop(g.t_conn);
  esp_timer_stop(g.t_retry);
  esp_timer_stop(g.t_rssi);
  esp_wifi_stop();
  if (g.h_wifi) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g.h_wifi);
  if (g.h_ip) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g.h_ip);
  g.h_wifi = g.h_ip = nullptr;
  esp_wifi_deinit();
  if (g.netif) {
    esp_wifi_clear_default_wifi_driver_and_handlers(g.netif);
    esp_netif_destroy(g.netif);
    g.netif = nullptr;
  }
  if (g.loop_ours) {
    esp_event_loop_delete_default();                            // frees the event task stack
    g.loop_ours = false;
  }
}

void doEnable() {
  {
    Lock l;
    if (g.active) return;
    g.hold = false;
    g.retry_n = 0;
  }
  esp_netif_init();
  const esp_err_t le = esp_event_loop_create_default();
  g.loop_ours = le == ESP_OK;
  g.netif = esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t e = g.netif ? esp_wifi_init(&cfg) : ESP_FAIL;
  if (e == ESP_OK) {
    {
      Lock l;
      g.active = true;
    }
    e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr,
                                            &g.h_wifi);
    if (e == ESP_OK)
      e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onIpEvent, nullptr,
                                              &g.h_ip);
    if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e == ESP_OK) e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_start();
  }
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "enable failed: %s", esp_err_to_name(e));
    teardownStack();
    {
      Lock l;
      g.want_on = false;
    }
    nvsSetFlag("en", false);
    notify(false);
    return;
  }
  nvsSetFlag("en", true);
  bool ch;
  {
    Lock l;
    ch = g.state == SVC_NET_OFF;
    if (ch) g.state = SVC_NET_IDLE;
  }
  if (ch) notify(false);
}

void doDisable() {
  const bool was = g.active;
  nvsSetFlag("en", false);
  if (!was) return;
  teardownStack();
  notify(true);
}

void doAuto() {
  Lock l;
  if (!g.active || !g.ready || !g.autoconnect || g.hold || nSaved() == 0 || g.link || g.attempt ||
      g.scanning || g.user_disc || g.pend)
    return;
  const bool quiet = g.retry_n > 0;
  g.scanning = g.auto_scan = true;
  bool ch = false;
  if (!quiet && g.state != SVC_NET_CONNECTING) {
    g.ssid[0] = 0;
    ch = setState(SVC_NET_CONNECTING, 0);
  }
  l.release();
  if (ch) notify(false);
  wifi_scan_config_t sc = scanCfg();
  if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
    Lock l2;
    g.scanning = g.auto_scan = false;
    if (g.state == SVC_NET_CONNECTING && !g.ssid[0]) setState(SVC_NET_IDLE, 0);
    scheduleRetry();
    l2.release();
    notify(false);
  }
}

void doScan() {
  wifi_scan_config_t sc = scanCfg();
  if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
    {
      Lock l;
      g.scanning = g.user_scan = false;
    }
    notify(true);
  }
}

void beginConnect(const Cmd& c) {
  wifi_config_t wc = {};
  strlcpy(reinterpret_cast<char*>(wc.sta.ssid), c.ssid, sizeof wc.sta.ssid);
  const char* pass = c.use_saved ? "" : c.pass;
  if (c.use_saved) {
    Lock l;
    if (Saved* s = findSaved(c.ssid)) pass = s->pass;
    strlcpy(reinterpret_cast<char*>(wc.sta.password), pass, sizeof wc.sta.password);
  } else {
    strlcpy(reinterpret_cast<char*>(wc.sta.password), pass, sizeof wc.sta.password);
  }
  wc.sta.threshold.authmode = wc.sta.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
  wc.sta.pmf_cfg.capable = true;
  wc.sta.pmf_cfg.required = false;

  bool was_scanning;
  {
    Lock l;
    was_scanning = g.scanning;
    g.scanning = g.auto_scan = g.user_scan = false;
  }
  if (was_scanning) esp_wifi_scan_stop();

  esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
  bool ch = false;
  {
    Lock l;
    if (!g.active) {
      memset(&wc, 0, sizeof wc);
      return;
    }
    g.cur_auto = c.automatic;
    g.remember_pending = c.remember && !c.automatic && !c.use_saved;
    strlcpy(g.cur_pass, reinterpret_cast<char*>(wc.sta.password), sizeof g.cur_pass);
    strlcpy(g.ssid, c.ssid, sizeof g.ssid);
    g.ip = 0;
    if (!c.automatic || g.retry_n == 0 || g.state != SVC_NET_FAILED) ch = setState(SVC_NET_CONNECTING, 0);
    g.attempt = true;
  }
  memset(&wc, 0, sizeof wc);
  if (e == ESP_OK) e = esp_wifi_connect();
  if (e != ESP_OK) {
    Lock l;
    g.attempt = false;
    g.remember_pending = false;
    ch = setState(SVC_NET_FAILED, SVC_NET_FAIL_OTHER);
  } else {
    esp_timer_stop(g.t_conn);
    esp_timer_start_once(g.t_conn, (uint64_t)kConnectTimeoutMs * 1000);
  }
  if (ch) notify(false);
}

void doConnect(const Cmd& c) {
  bool busy;
  {
    Lock l;
    if (!g.active || !g.ready) return;
    if (c.automatic && (g.hold || g.link || g.attempt || g.user_disc)) return;
    busy = g.link || g.attempt;
    if (busy && !g.user_disc) {
      g.pend = true;
      g.pend_cmd = c;
      g.user_disc = true;
    } else if (busy) {                                           // a disconnect is already running
      g.pend = true;
      g.pend_cmd = c;
      return;
    }
  }
  if (busy) {
    esp_timer_stop(g.t_conn);
    if (esp_wifi_disconnect() == ESP_OK) {
      esp_timer_start_once(g.t_conn, (uint64_t)kDiscWatchdogMs * 1000);
      return;                                                   // the event handler resumes us
    }
    Lock l;
    g.user_disc = g.pend = false;
    g.link = g.attempt = false;
  }
  beginConnect(c);
}

void doDisconnect() {
  bool busy, ch = false;
  {
    Lock l;
    if (!g.active) return;
    g.hold = true;
    g.pend = false;
    esp_timer_stop(g.t_retry);
    busy = g.link || g.attempt;
    if (busy) {
      g.user_disc = true;
    } else {
      ch = g.state != SVC_NET_IDLE || g.ssid[0];
      g.state = SVC_NET_IDLE;
      g.fail = 0;
      g.ssid[0] = 0;
    }
  }
  if (busy) {
    if (esp_wifi_disconnect() == ESP_OK) {
      esp_timer_stop(g.t_conn);
      esp_timer_start_once(g.t_conn, (uint64_t)kDiscWatchdogMs * 1000);
    } else {
      Lock l;
      g.user_disc = false;
      g.link = g.attempt = false;
      g.state = SVC_NET_IDLE;
      g.ssid[0] = 0;
      ch = true;
    }
  }
  if (ch) notify(false);
}

void doForget(const Cmd& c) {
  bool is_cur;
  {
    Lock l;
    is_cur = g.active && strcmp(g.ssid, c.ssid) == 0 &&
             (g.link || g.attempt || g.state == SVC_NET_FAILED);
  }
  if (is_cur) doDisconnect();
  nvsPersistSaved();
  notify(true);
}

void workerMain(void*) {
  Cmd c;
  for (;;) {
    if (xQueueReceive(g.q, &c, pdMS_TO_TICKS(500)) != pdTRUE) {
      xSemaphoreTake(g.qmu, portMAX_DELAY);
      if (!g.active && uxQueueMessagesWaiting(g.q) == 0) {
        g.worker_alive = false;
        xSemaphoreGive(g.qmu);
        vTaskDelete(nullptr);
      }
      xSemaphoreGive(g.qmu);
      continue;
    }
    switch (c.type) {
      case CmdType::Enable: doEnable(); break;
      case CmdType::Disable: doDisable(); break;
      case CmdType::Scan: doScan(); break;
      case CmdType::Connect: doConnect(c); break;
      case CmdType::Disconnect: doDisconnect(); break;
      case CmdType::Forget: doForget(c); break;
      case CmdType::Auto: doAuto(); break;
      case CmdType::Save:
        nvsPersistSaved();
        break;
    }
    memset(&c, 0, sizeof c);
  }
}

}  // namespace

// ---- public API ----------------------------------------------------------------------------
extern "C" {

void svc_net_init(svc_net_cb_t cb) {
  g.cb = cb;
  g.mu = xSemaphoreCreateMutex();
  g.qmu = xSemaphoreCreateMutex();
  g.q = xQueueCreateWithCaps(8, sizeof(Cmd), MALLOC_CAP_SPIRAM);
  g.big = static_cast<Big*>(heap_caps_calloc(1, sizeof(Big), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!g.mu || !g.qmu || !g.q || !g.big) {
    ESP_LOGE(TAG, "out of memory");
    g.cb = nullptr;
    return;
  }
  esp_timer_create_args_t a = {};
  a.dispatch_method = ESP_TIMER_TASK;
  a.name = "net_conn";
  a.callback = onConnTimer;
  esp_timer_create(&a, &g.t_conn);
  a.name = "net_retry";
  a.callback = onRetryTimer;
  esp_timer_create(&a, &g.t_retry);
  a.name = "net_rssi";
  a.callback = onRssiTimer;
  esp_timer_create(&a, &g.t_rssi);
  nvsLoad();
  if (g.want_on) post(CmdType::Enable);
}

bool svc_net_set_enabled(bool on) {
  if (!g.big) return false;
  {
    Lock l;
    g.want_on = on;
  }
  return post(on ? CmdType::Enable : CmdType::Disable);
}

bool svc_net_enabled(void) { return g.want_on; }

bool svc_net_scan(void) {
  if (!g.big) return false;
  bool ch = false;
  {
    Lock l;
    if (!g.active || !g.ready) return false;
    if (g.scanning) {
      g.user_scan = true;
      return true;
    }
    if (g.attempt || g.user_disc || g.state == SVC_NET_CONNECTING) return false;
    g.scanning = g.user_scan = true;
    ch = g.state == SVC_NET_IDLE || g.state == SVC_NET_FAILED;
  }
  if (!post(CmdType::Scan)) {
    Lock l;
    g.scanning = g.user_scan = false;
    return false;
  }
  if (ch) notify(false);
  return true;
}

int svc_net_scan_results(svc_net_ap_t* out, int max) {
  if (!g.big) return 0;
  Lock l;
  int n = g.big->nres < max ? g.big->nres : max;
  for (int i = 0; i < n; i++) {
    out[i] = g.big->res[i];
    out[i].saved = findSaved(out[i].ssid) != nullptr;
  }
  return n;
}

bool svc_net_connect(const char* ssid, const char* password, bool remember) {
  if (!g.big || !ssid || !ssid[0]) return false;
  Cmd c{};
  c.type = CmdType::Connect;
  c.remember = remember;
  c.use_saved = password == nullptr;
  strlcpy(c.ssid, ssid, sizeof c.ssid);
  if (password) strlcpy(c.pass, password, sizeof c.pass);
  {
    Lock l;
    if (!g.active || !g.ready) return false;
    g.hold = false;
    g.retry_n = 0;
    esp_timer_stop(g.t_retry);
    g.auto_scan = false;
    strlcpy(g.ssid, ssid, sizeof g.ssid);                       // shown right away
    g.ip = 0;
    setState(SVC_NET_CONNECTING, 0);
  }
  const bool ok = post(c);
  memset(&c, 0, sizeof c);
  notify(false);
  return ok;
}

void svc_net_disconnect(void) {
  if (g.big) post(CmdType::Disconnect);
}

void svc_net_forget(const char* ssid) {
  if (!g.big || !ssid) return;
  Cmd c{};
  c.type = CmdType::Forget;
  strlcpy(c.ssid, ssid, sizeof c.ssid);
  {
    Lock l;
    if (Saved* s = findSaved(ssid)) memset(s, 0, sizeof *s);
  }
  post(c);
}

void svc_net_status(svc_net_status_t* out) {
  memset(out, 0, sizeof *out);
  if (!g.big) return;
  Lock l;
  out->state = g.state;
  if ((g.state == SVC_NET_IDLE || g.state == SVC_NET_FAILED) && g.scanning && g.user_scan)
    out->state = SVC_NET_SCANNING;
  strlcpy(out->ssid, g.ssid, sizeof out->ssid);
  out->rssi = g.rssi;
  out->ip = g.ip;
  out->fail = g.fail;
}

}  // extern "C"
