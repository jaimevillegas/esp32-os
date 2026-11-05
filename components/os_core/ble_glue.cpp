// Glue between svc_ble and the framework (Ble for apps, Topic::Ble events, status bar icon).
//
// Topic::Ble payload: bits 0-7 BleState, bit 8 = a scan finished. Published on every state
// change. The stack is only created while Bluetooth is enabled; the flag is persisted.
#include <cstring>

#include "core_internal.h"
#include "esp_log.h"
#include "os_ui.h"
#include "svc_ble.h"

namespace os::core {

static const char* const TAG = "ble";

static BleState toState(svc_ble_state_t s) {
  switch (s) {
    case SVC_BLE_IDLE: return BleState::Idle;
    case SVC_BLE_SCANNING: return BleState::Scanning;
    case SVC_BLE_CONNECTING: return BleState::Connecting;
    case SVC_BLE_CONNECTED: return BleState::Connected;
    default: return BleState::Off;
  }
}

static void onBleEvent(bool scan_done) {
  const svc_ble_state_t st = svc_ble_state();
  // Icon: hidden while off, plain when on, highlighted while a device is connected.
  os_ui_set_bt(st == SVC_BLE_OFF ? 0 : (svc_ble_connected_count() > 0 ? 2 : 1));
  Msg m{};
  m.topic = Topic::Ble;
  m.arg = (uint32_t)toState(st) | (scan_done ? 0x100u : 0u);
  bus().publish(m);
}

class BleImpl final : public Ble {
 public:
  bool setEnabled(bool on) override {
    if (on == svc_ble_enabled()) return true;
    systemKv().setU32("ble_on", on ? 1 : 0);
    ESP_LOGI(TAG, "%s", on ? "enable" : "disable");
    return svc_ble_set_enabled(on);
  }
  bool enabled() const override { return svc_ble_enabled(); }
  bool startScan(uint32_t duration_ms) override { return svc_ble_scan(duration_ms); }
  void stopScan() override { svc_ble_stop_scan(); }

  int devices(BleDevice* out, int max) override {
    static_assert(sizeof(BleDevice) == sizeof(svc_ble_dev_t), "layouts must match");
    // BleDevice mirrors svc_ble_dev_t field for field: take the snapshot straight into it.
    return svc_ble_devices(reinterpret_cast<svc_ble_dev_t*>(out), max);
  }

  bool connect(const BleDevice& d) override { return svc_ble_connect(d.addr, d.addr_type); }
  void disconnect(const BleDevice& d) override { svc_ble_disconnect(d.addr, d.addr_type); }
  void forget(const BleDevice& d) override { svc_ble_forget(d.addr, d.addr_type); }
  BleState state() const override { return toState(svc_ble_state()); }
};

static BleImpl s_ble;

Ble* bleService() { return &s_ble; }

void bleStart() {
  svc_ble_init(onBleEvent);
  uint32_t on = 0;
  if (systemKv().getU32("ble_on", on) && on) svc_ble_set_enabled(true);
}

}  // namespace os::core
