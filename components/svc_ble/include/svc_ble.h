#pragma once
// BLE central service on top of NimBLE. BLE only: the ESP32-S3 has no Bluetooth Classic.
//
// The radio stack is created only while the service is enabled and is completely released
// (host and controller) when it is disabled. Every function is safe to call from any task
// except from inside the notify callback.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SVC_BLE_OFF = 0,     // stack not running (or still starting)
  SVC_BLE_IDLE,
  SVC_BLE_SCANNING,
  SVC_BLE_CONNECTING,  // connecting or pairing
  SVC_BLE_CONNECTED,
} svc_ble_state_t;

typedef struct {
  char name[32];       // empty when unknown
  uint8_t addr[6];     // as in ble_addr_t.val: least significant byte first
  uint8_t addr_type;   // BLE_ADDR_* of the underlying stack
  int8_t rssi;         // -127 when the device was not seen in the current session
  bool connectable;
  bool bonded;
  bool connected;
  int8_t battery;      // 0..100, -1 unknown
} svc_ble_dev_t;

// Called from the stack's tasks after every state change and when a scan finishes.
typedef void (*svc_ble_notify_fn)(bool scan_done);

void svc_ble_init(svc_ble_notify_fn cb);
// Asynchronous: the state becomes IDLE once the stack is up. Returns false only when the
// request cannot be queued.
bool svc_ble_set_enabled(bool on);
bool svc_ble_enabled(void);      // what was requested (true while starting)
svc_ble_state_t svc_ble_state(void);
int svc_ble_connected_count(void);

bool svc_ble_scan(uint32_t duration_ms);
void svc_ble_stop_scan(void);
// Snapshot sorted: connected, bonded, then by signal strength.
int svc_ble_devices(svc_ble_dev_t *out, int max);
bool svc_ble_connect(const uint8_t addr[6], uint8_t addr_type);
void svc_ble_disconnect(const uint8_t addr[6], uint8_t addr_type);
void svc_ble_forget(const uint8_t addr[6], uint8_t addr_type);
void svc_ble_set_auto_reconnect(bool on);

#ifdef __cplusplus
}
#endif
