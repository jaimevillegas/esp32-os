#pragma once
// Wi-Fi station service (ESP-IDF esp_wifi + esp_netif). Plain C API, independent of the
// framework: os_core/net_glue.cpp is the only user and adapts it to os::Net.
//
// The radio stack is created only while the user has Wi-Fi switched on and is destroyed
// again when it is switched off. Every call is non-blocking: work is done by a small worker
// task (alive only while needed) and by the esp_event handlers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Values match os::WifiState.
typedef enum {
    SVC_NET_OFF = 0,
    SVC_NET_IDLE,
    SVC_NET_SCANNING,
    SVC_NET_CONNECTING,
    SVC_NET_CONNECTED,
    SVC_NET_FAILED,
} svc_net_state_t;

// Failure reasons reported in svc_net_status_t.fail (valid in SVC_NET_FAILED).
typedef enum {
    SVC_NET_FAIL_NONE = 0,
    SVC_NET_FAIL_WRONG_PASSWORD = 1,
    SVC_NET_FAIL_NOT_FOUND = 2,
    SVC_NET_FAIL_TIMEOUT = 3,
    SVC_NET_FAIL_OTHER = 4,
} svc_net_fail_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured;
    bool saved;
} svc_net_ap_t;

typedef struct {
    svc_net_state_t state;
    char ssid[33];
    int8_t rssi;
    uint32_t ip;          // IPv4, host order
    uint8_t fail;         // svc_net_fail_t
} svc_net_status_t;

// Called (from any task, never with a lock held) after every state change and scan
// completion. `scan_done` is true when a scan finished.
typedef void (*svc_net_cb_t)(bool scan_done);

// Loads the saved networks and the persisted on/off flag; if Wi-Fi was left on, brings the
// radio up in the background. Call once, after NVS is initialised.
void svc_net_init(svc_net_cb_t cb);

bool svc_net_set_enabled(bool on);       // persisted
bool svc_net_enabled(void);
bool svc_net_scan(void);                 // async; false when the radio is off or busy connecting
int  svc_net_scan_results(svc_net_ap_t *out, int max);   // dedup by SSID, strongest first
// password NULL = use the saved one (or open network when none is saved).
bool svc_net_connect(const char *ssid, const char *password, bool remember);
void svc_net_disconnect(void);
void svc_net_forget(const char *ssid);
void svc_net_status(svc_net_status_t *out);

#ifdef __cplusplus
}
#endif
