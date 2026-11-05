// BLE central service (NimBLE). See svc_ble.h.
//
// Threading: NimBLE callbacks run in the host task, the public API is called from the UI or
// the worker task. All shared state is behind s_mu; NimBLE calls are made without holding it.
#include "svc_ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "store/config/ble_store_config.h"

#define TAG "svc_ble"

void ble_store_config_init(void);   // NVS-backed bond store; not declared in the public header

#define MAX_DEVS 40          // 32 scan results plus up to 8 bonded devices
#define MAX_CONN 2
#define NO_RSSI (-127)
#define NAMES_NS "ble_names"

typedef struct {
  bool used;
  uint16_t handle;
  uint8_t addr[6];
  uint8_t type;
  bool ready;                // link up, security and the initial reads finished
  bool reads_started;
} conn_t;

static SemaphoreHandle_t s_mu;
static SemaphoreHandle_t s_sync_sem;
static svc_ble_notify_fn s_cb;
static esp_timer_handle_t s_auto_timer;

static svc_ble_dev_t *s_devs;       // PSRAM, only while the stack is up
static int s_ndev;
static conn_t s_conn[MAX_CONN];

static bool s_want;                 // requested on/off
static bool s_busy;                 // worker task running
static bool s_up;                   // host synced
static bool s_scanning;
static bool s_connect_pending;
static bool s_auto = true;
static uint8_t s_own_type;
static uint8_t s_auto_addr[6], s_auto_type;
static uint8_t s_supp_addr[6], s_supp_type = 0xFF;   // explicitly disconnected: no auto-reconnect

static const ble_uuid16_t kUuidName = BLE_UUID16_INIT(0x2A00);
static const ble_uuid16_t kUuidBattery = BLE_UUID16_INIT(0x2A19);

static int gap_cb(struct ble_gap_event *ev, void *arg);

static inline void lock(void) { xSemaphoreTake(s_mu, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_mu); }
static void notify(bool scan_done) {
  if (s_cb) s_cb(scan_done);
}

// ---- persisted names of bonded devices ----------------------------------------------------
static void name_key(const uint8_t a[6], char key[13]) {
  snprintf(key, 13, "%02x%02x%02x%02x%02x%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static void name_save(const uint8_t a[6], const char *name) {
  nvs_handle_t h;
  if (nvs_open(NAMES_NS, NVS_READWRITE, &h) != ESP_OK) return;
  char key[13];
  name_key(a, key);
  nvs_set_str(h, key, name);
  nvs_commit(h);
  nvs_close(h);
}

static void name_load(const uint8_t a[6], char *out, size_t cap) {
  out[0] = 0;
  nvs_handle_t h;
  if (nvs_open(NAMES_NS, NVS_READONLY, &h) != ESP_OK) return;
  char key[13];
  name_key(a, key);
  size_t n = cap;
  if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = 0;
  nvs_close(h);
}

static void name_erase(const uint8_t a[6]) {
  nvs_handle_t h;
  if (nvs_open(NAMES_NS, NVS_READWRITE, &h) != ESP_OK) return;
  char key[13];
  name_key(a, key);
  nvs_erase_key(h, key);
  nvs_commit(h);
  nvs_close(h);
}

// ---- device table (call with s_mu held) ---------------------------------------------------
static int find_dev(const uint8_t a[6], uint8_t type) {
  for (int i = 0; i < s_ndev; i++)
    if (s_devs[i].addr_type == type && memcmp(s_devs[i].addr, a, 6) == 0) return i;
  return -1;
}

static int add_dev(const uint8_t a[6], uint8_t type) {
  if (s_ndev >= MAX_DEVS) return -1;
  svc_ble_dev_t *d = &s_devs[s_ndev];
  memset(d, 0, sizeof *d);
  memcpy(d->addr, a, 6);
  d->addr_type = type;
  d->rssi = NO_RSSI;
  d->battery = -1;
  return s_ndev++;
}

static void remove_dev(int i) {
  memmove(&s_devs[i], &s_devs[i + 1], (size_t)(s_ndev - i - 1) * sizeof *s_devs);
  s_ndev--;
}

static conn_t *conn_by_handle(uint16_t h) {
  for (int i = 0; i < MAX_CONN; i++)
    if (s_conn[i].used && s_conn[i].handle == h) return &s_conn[i];
  return NULL;
}

static conn_t *conn_by_addr(const uint8_t a[6], uint8_t type) {
  for (int i = 0; i < MAX_CONN; i++)
    if (s_conn[i].used && s_conn[i].type == type && memcmp(s_conn[i].addr, a, 6) == 0)
      return &s_conn[i];
  return NULL;
}

static svc_ble_state_t state_locked(void) {
  if (!s_up) return SVC_BLE_OFF;
  bool any_ready = false;
  for (int i = 0; i < MAX_CONN; i++) {
    if (!s_conn[i].used) continue;
    if (!s_conn[i].ready) return SVC_BLE_CONNECTING;
    any_ready = true;
  }
  if (s_connect_pending) return SVC_BLE_CONNECTING;
  if (s_scanning) return SVC_BLE_SCANNING;
  return any_ready ? SVC_BLE_CONNECTED : SVC_BLE_IDLE;
}

// Adds bonded peers that are not in the table and refreshes the bonded flag.
static void refresh_bonds(void) {
  ble_addr_t peers[8];
  int n = 0;
  if (ble_store_util_bonded_peers(peers, &n, 8) != 0) n = 0;
  lock();
  if (s_devs) {
    for (int i = 0; i < s_ndev; i++) s_devs[i].bonded = false;
    for (int k = 0; k < n; k++) {
      int i = find_dev(peers[k].val, peers[k].type);
      if (i < 0) {
        i = add_dev(peers[k].val, peers[k].type);
        if (i < 0) continue;
        name_load(peers[k].val, s_devs[i].name, sizeof s_devs[i].name);
        s_devs[i].connectable = true;
      }
      s_devs[i].bonded = true;
    }
  }
  unlock();
}

// ---- GATT reads after connecting ----------------------------------------------------------
static void finish_reads(uint16_t handle) {
  lock();
  conn_t *c = conn_by_handle(handle);
  if (c && !c->ready) {
    c->ready = true;
    int i = find_dev(c->addr, c->type);
    if (i >= 0) s_devs[i].connected = true;
  }
  unlock();
  notify(false);
}

static int battery_cb(uint16_t ch, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                      void *arg) {
  uint16_t handle = (uint16_t)(uintptr_t)arg;
  if (err->status == 0 && attr && attr->om && OS_MBUF_PKTLEN(attr->om) >= 1) {
    uint8_t v = 0;
    os_mbuf_copydata(attr->om, 0, 1, &v);
    lock();
    conn_t *c = conn_by_handle(handle);
    if (c && s_devs) {
      int i = find_dev(c->addr, c->type);
      if (i >= 0 && v <= 100) s_devs[i].battery = (int8_t)v;
    }
    unlock();
    return 0;
  }
  finish_reads(handle);     // BLE_HS_EDONE or an error: nothing more to read
  return 0;
}

static void read_battery(uint16_t handle) {
  int rc = ble_gattc_read_by_uuid(handle, 1, 0xffff, &kUuidBattery.u, battery_cb,
                                  (void *)(uintptr_t)handle);
  if (rc != 0) finish_reads(handle);
}

static int name_cb(uint16_t ch, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                   void *arg) {
  uint16_t handle = (uint16_t)(uintptr_t)arg;
  if (err->status == 0 && attr && attr->om) {
    char buf[32] = {0};
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len > sizeof buf - 1) len = sizeof buf - 1;
    os_mbuf_copydata(attr->om, 0, len, buf);
    if (buf[0]) {
      uint8_t addr[6] = {0};
      bool save = false;
      lock();
      conn_t *c = conn_by_handle(handle);
      if (c && s_devs) {
        int i = find_dev(c->addr, c->type);
        if (i >= 0) {
          memcpy(s_devs[i].name, buf, sizeof s_devs[i].name);
          save = s_devs[i].bonded;
          memcpy(addr, s_devs[i].addr, 6);
        }
      }
      unlock();
      if (save) name_save(addr, buf);
    }
    return 0;
  }
  read_battery(handle);
  return 0;
}

static void start_reads(uint16_t handle) {
  lock();
  conn_t *c = conn_by_handle(handle);
  bool go = c && !c->reads_started;
  if (go) c->reads_started = true;
  unlock();
  if (!go) return;
  int rc = ble_gattc_read_by_uuid(handle, 1, 0xffff, &kUuidName.u, name_cb,
                                  (void *)(uintptr_t)handle);
  if (rc != 0) read_battery(handle);
}

// ---- GAP events ---------------------------------------------------------------------------
static void on_connect(int status, uint16_t handle) {
  if (status != 0) {
    lock();
    s_connect_pending = false;
    unlock();
    ESP_LOGW(TAG, "connect failed: %d", status);
    notify(false);
    return;
  }
  struct ble_gap_conn_desc d;
  bool ok = ble_gap_conn_find(handle, &d) == 0;
  lock();
  s_connect_pending = false;
  conn_t *c = NULL;
  for (int i = 0; ok && i < MAX_CONN; i++)
    if (!s_conn[i].used) {
      c = &s_conn[i];
      break;
    }
  if (c) {
    memset(c, 0, sizeof *c);
    c->used = true;
    c->handle = handle;
    memcpy(c->addr, d.peer_ota_addr.val, 6);
    c->type = d.peer_ota_addr.type;
  }
  unlock();
  if (!c) {
    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    notify(false);
    return;
  }
  notify(false);
  // Pair (or encrypt with the stored bond). The reads start when that finishes.
  if (ble_gap_security_initiate(handle) != 0) start_reads(handle);
}

static void on_disconnect(uint16_t handle, int reason) {
  lock();
  conn_t *c = conn_by_handle(handle);
  if (c) {
    int i = find_dev(c->addr, c->type);
    if (i >= 0) {
      s_devs[i].connected = false;
      s_devs[i].battery = -1;
    }
    c->used = false;
  }
  const bool up = s_up;
  unlock();
  ESP_LOGI(TAG, "disconnected, reason %d", reason);
  if (up) refresh_bonds();
  notify(false);
}

static void on_enc_change(uint16_t handle, int status) {
  struct ble_gap_conn_desc d;
  if (status == 0 && ble_gap_conn_find(handle, &d) == 0 && d.sec_state.bonded) {
    char name[32] = {0};
    uint8_t id[6];
    memcpy(id, d.peer_id_addr.val, 6);
    lock();
    conn_t *c = conn_by_handle(handle);
    if (c && s_devs) {
      int i = find_dev(c->addr, c->type);
      // Bonds are stored under the identity address: make the table entry use it.
      int j = find_dev(id, d.peer_id_addr.type);
      if (i >= 0 && j >= 0 && j != i) {
        if (!s_devs[i].name[0]) memcpy(s_devs[i].name, s_devs[j].name, sizeof s_devs[i].name);
        remove_dev(j);
        i = find_dev(c->addr, c->type);
      }
      memcpy(c->addr, id, 6);
      c->type = d.peer_id_addr.type;
      if (i >= 0) {
        memcpy(s_devs[i].addr, id, 6);
        s_devs[i].addr_type = d.peer_id_addr.type;
        s_devs[i].bonded = true;
        memcpy(name, s_devs[i].name, sizeof name);
      }
    }
    unlock();
    if (name[0]) name_save(id, name);
  }
  if (status == 0 || status == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_PAIR_NOT_SUPP)) {
    // Encrypted, or the peer simply has no pairing: carry on unbonded.
    start_reads(handle);
  } else {
    ESP_LOGW(TAG, "pairing failed: %d", status);
    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
  }
}

static void auto_timer_cb(void *arg) { svc_ble_connect(s_auto_addr, s_auto_type); }

static void on_disc(const struct ble_gap_disc_desc *d) {
  struct ble_hs_adv_fields f;
  memset(&f, 0, sizeof f);
  ble_hs_adv_parse_fields(&f, d->data, d->length_data);
  const bool scan_rsp = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
  const bool conn_ev = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                       d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
  bool trigger = false;
  lock();
  if (s_devs && s_up) {
    int i = find_dev(d->addr.val, d->addr.type);
    if (i < 0) i = add_dev(d->addr.val, d->addr.type);
    if (i >= 0) {
      svc_ble_dev_t *e = &s_devs[i];
      e->rssi = d->rssi;
      if (conn_ev) e->connectable = true;
      if (f.name && f.name_len > 0 && (!scan_rsp || !e->name[0] || f.name_is_complete)) {
        size_t n = f.name_len < sizeof e->name - 1 ? f.name_len : sizeof e->name - 1;
        memcpy(e->name, f.name, n);
        e->name[n] = 0;
      }
      bool free_slot = false;
      for (int k = 0; k < MAX_CONN; k++) free_slot |= !s_conn[k].used;
      const bool suppressed = s_supp_type == e->addr_type && memcmp(s_supp_addr, e->addr, 6) == 0;
      if (s_auto && e->bonded && !e->connected && conn_ev && !s_connect_pending && free_slot &&
          !suppressed) {
        memcpy(s_auto_addr, e->addr, 6);
        s_auto_type = e->addr_type;
        trigger = true;
      }
    }
  }
  unlock();
  // Not from this callback: connecting cancels the scan that is delivering this event.
  if (trigger) esp_timer_start_once(s_auto_timer, 1000);
}

static int gap_cb(struct ble_gap_event *ev, void *arg) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      on_connect(ev->connect.status, ev->connect.conn_handle);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      on_disconnect(ev->disconnect.conn.conn_handle, ev->disconnect.reason);
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      on_enc_change(ev->enc_change.conn_handle, ev->enc_change.status);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // The peer lost its bond: drop ours and pair again.
      struct ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
        ble_store_util_delete_peer(&d.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      // Passkey entry/display and numeric comparison need UI that the API does not offer.
      ESP_LOGW(TAG, "peer demands a passkey (action %d): not supported", ev->passkey.params.action);
      ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      break;
    default:
      break;
  }
  return 0;
}

static int disc_cb(struct ble_gap_event *ev, void *arg) {
  if (ev->type == BLE_GAP_EVENT_DISC) {
    on_disc(&ev->disc);
  } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    lock();
    bool was = s_scanning;
    s_scanning = false;
    unlock();
    if (was) notify(true);
  }
  return 0;
}

// ---- stack bring-up / tear-down -----------------------------------------------------------
static void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  if (ble_hs_id_infer_auto(0, &s_own_type) != 0) s_own_type = BLE_OWN_ADDR_PUBLIC;
  if (s_sync_sem) xSemaphoreGive(s_sync_sem);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason %d", reason); }

static void host_task(void *arg) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void free_table(void) {
  lock();
  heap_caps_free(s_devs);
  s_devs = NULL;
  s_ndev = 0;
  memset(s_conn, 0, sizeof s_conn);
  unlock();
  if (s_sync_sem) {
    vSemaphoreDelete(s_sync_sem);
    s_sync_sem = NULL;
  }
}

static void stack_down(void) {
  lock();
  s_up = false;
  s_scanning = false;
  s_connect_pending = false;
  unlock();
  nimble_port_stop();               // stops the host; links are terminated
  nimble_port_deinit();             // host and controller deinit
  free_table();
}

static bool stack_up(void) {
  const uint32_t i0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t p0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  lock();
  s_devs = heap_caps_calloc(MAX_DEVS, sizeof *s_devs, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_ndev = 0;
  memset(s_conn, 0, sizeof s_conn);
  s_scanning = s_connect_pending = false;
  s_supp_type = 0xFF;
  unlock();
  s_sync_sem = xSemaphoreCreateBinary();
  if (!s_devs || !s_sync_sem) {
    free_table();
    return false;
  }
  if (nimble_port_init() != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed");
    free_table();
    return false;
  }
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;     // Just Works
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_store_config_init();
  nimble_port_freertos_init(host_task);
  if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "host did not sync");
    stack_down();
    return false;
  }
  lock();
  s_up = true;
  unlock();
  refresh_bonds();
  ESP_LOGI(TAG, "stack up: %d B internal, %d B PSRAM used",
           (int)(i0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           (int)(p0 - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  return true;
}

static void worker(void *arg) {
  for (;;) {
    lock();
    const bool want = s_want, up = s_up;
    if (want == up) {
      s_busy = false;
      unlock();
      break;
    }
    unlock();
    if (want) {
      if (!stack_up()) {
        lock();
        s_want = false;           // could not start: report off
        unlock();
      }
    } else {
      stack_down();
      ESP_LOGI(TAG, "stack down: %u B internal free",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    notify(false);
  }
  vTaskDelete(NULL);
}

// ---- public API ---------------------------------------------------------------------------
void svc_ble_init(svc_ble_notify_fn cb) {
  s_cb = cb;
  if (!s_mu) s_mu = xSemaphoreCreateMutex();
  if (!s_auto_timer) {
    const esp_timer_create_args_t a = {.callback = auto_timer_cb, .name = "ble_auto"};
    esp_timer_create(&a, &s_auto_timer);
  }
}

bool svc_ble_set_enabled(bool on) {
  lock();
  s_want = on;
  const bool spawn = !s_busy && s_want != s_up;
  if (spawn) s_busy = true;
  unlock();
  if (spawn && xTaskCreate(worker, "ble_ctl", 4096, NULL, 5, NULL) != pdPASS) {
    lock();
    s_busy = false;
    s_want = s_up;
    unlock();
    return false;
  }
  return true;
}

bool svc_ble_enabled(void) { return s_want; }

svc_ble_state_t svc_ble_state(void) {
  lock();
  svc_ble_state_t st = state_locked();
  unlock();
  return st;
}

int svc_ble_connected_count(void) {
  int n = 0;
  lock();
  for (int i = 0; i < MAX_CONN; i++) n += s_conn[i].used && s_conn[i].ready;
  unlock();
  return n;
}

void svc_ble_set_auto_reconnect(bool on) { s_auto = on; }

bool svc_ble_scan(uint32_t duration_ms) {
  lock();
  const bool ok = s_up && s_devs && !s_scanning && !s_connect_pending;
  if (ok) {
    // Drop stale results; keep connected and bonded entries.
    for (int i = s_ndev - 1; i >= 0; i--) {
      if (s_devs[i].connected || s_devs[i].bonded) {
        s_devs[i].rssi = NO_RSSI;
        s_devs[i].connectable = s_devs[i].bonded;
      } else {
        remove_dev(i);
      }
    }
    s_scanning = true;
  }
  unlock();
  if (!ok) return false;
  struct ble_gap_disc_params p;
  memset(&p, 0, sizeof p);
  p.filter_duplicates = 1;
  p.passive = 0;                       // active: also asks for the scan response (names)
  const int32_t dur = duration_ms ? (int32_t)duration_ms : 10000;
  if (ble_gap_disc(s_own_type, dur, &p, disc_cb, NULL) != 0) {
    lock();
    s_scanning = false;
    unlock();
    return false;
  }
  notify(false);
  return true;
}

void svc_ble_stop_scan(void) {
  lock();
  const bool was = s_scanning && s_up;
  s_scanning = false;
  unlock();
  if (!was) return;
  ble_gap_disc_cancel();               // cancelling raises no event
  notify(true);
}

int svc_ble_devices(svc_ble_dev_t *out, int max) {
  int n = 0;
  lock();
  if (s_devs) {
    // Selection by rank: connected, bonded, then by RSSI. The table is small (<= 40).
    bool taken[MAX_DEVS] = {false};
    while (n < max && n < s_ndev) {
      int best = -1;
      for (int i = 0; i < s_ndev; i++) {
        if (taken[i]) continue;
        if (best < 0) {
          best = i;
          continue;
        }
        const svc_ble_dev_t *a = &s_devs[i], *b = &s_devs[best];
        const int ra = a->connected ? 2 : a->bonded ? 1 : 0;
        const int rb = b->connected ? 2 : b->bonded ? 1 : 0;
        if (ra > rb || (ra == rb && a->rssi > b->rssi)) best = i;
      }
      taken[best] = true;
      out[n++] = s_devs[best];
    }
  }
  unlock();
  return n;
}

bool svc_ble_connect(const uint8_t addr[6], uint8_t addr_type) {
  bool was_scanning = false;
  lock();
  bool ok = s_up && !s_connect_pending && !conn_by_addr(addr, addr_type);
  int used = 0;
  for (int i = 0; i < MAX_CONN; i++) used += s_conn[i].used;
  ok = ok && used < MAX_CONN;
  if (ok) {
    was_scanning = s_scanning;
    s_scanning = false;
    s_connect_pending = true;
    if (s_supp_type == addr_type && memcmp(s_supp_addr, addr, 6) == 0) s_supp_type = 0xFF;
  }
  unlock();
  if (!ok) return false;
  if (was_scanning) {
    ble_gap_disc_cancel();
    notify(true);
  }
  ble_addr_t peer;
  peer.type = addr_type;
  memcpy(peer.val, addr, 6);
  const int rc = ble_gap_connect(s_own_type, &peer, 10000, NULL, gap_cb, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "ble_gap_connect: %d", rc);
    lock();
    s_connect_pending = false;
    unlock();
    notify(false);
    return false;
  }
  notify(false);
  return true;
}

void svc_ble_disconnect(const uint8_t addr[6], uint8_t addr_type) {
  uint16_t h = 0;
  bool found = false;
  lock();
  conn_t *c = conn_by_addr(addr, addr_type);
  if (c) {
    h = c->handle;
    found = true;
    memcpy(s_supp_addr, addr, 6);
    s_supp_type = addr_type;
  }
  unlock();
  if (found) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}

void svc_ble_forget(const uint8_t addr[6], uint8_t addr_type) {
  svc_ble_disconnect(addr, addr_type);
  lock();
  const bool up = s_up;
  unlock();
  if (!up) return;
  ble_addr_t a;
  a.type = addr_type;
  memcpy(a.val, addr, 6);
  ble_store_util_delete_peer(&a);
  name_erase(addr);
  lock();
  if (s_devs) {
    int i = find_dev(addr, addr_type);
    if (i >= 0) {
      s_devs[i].bonded = false;
      if (s_devs[i].rssi == NO_RSSI && !s_devs[i].connected) remove_dev(i);
    }
  }
  unlock();
  notify(false);
}
