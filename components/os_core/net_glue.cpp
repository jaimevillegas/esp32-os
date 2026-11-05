// Glue between the Wi-Fi service (svc_net) and the framework: implements os::Net, publishes
// Topic::Network and drives the status-bar Wi-Fi icon.
#include <cstring>

#include "core_internal.h"
#include "os_ui.h"
#include "svc_net.h"

namespace os::core {

namespace {

class NetImpl final : public Net {
 public:
  bool setEnabled(bool on) override { return svc_net_set_enabled(on); }
  bool enabled() const override { return svc_net_enabled(); }
  bool scan() override { return svc_net_scan(); }

  int scanResults(WifiAp* out, int max) override {
    svc_net_ap_t tmp[24];
    const int n = svc_net_scan_results(tmp, max < 24 ? max : 24);
    for (int i = 0; i < n; i++) {
      strlcpy(out[i].ssid, tmp[i].ssid, sizeof out[i].ssid);
      out[i].rssi = tmp[i].rssi;
      out[i].secured = tmp[i].secured;
      out[i].saved = tmp[i].saved;
    }
    return n;
  }

  bool connect(const char* ssid, const char* password, bool remember) override {
    return svc_net_connect(ssid, password, remember);
  }
  void disconnect() override { svc_net_disconnect(); }
  void forget(const char* ssid) override { svc_net_forget(ssid); }

  void status(WifiStatus& out) const override {
    svc_net_status_t s;
    svc_net_status(&s);
    out.state = static_cast<WifiState>(s.state);
    strlcpy(out.ssid, s.ssid, sizeof out.ssid);
    out.rssi = s.rssi;
    out.ip = s.ip;
    out.fail_reason = s.fail;
  }
};

NetImpl s_net;
uint32_t s_version;

// Status-bar level: -1 radio off, 0 not connected, 1..3 signal bars.
int barLevel(const svc_net_status_t& s) {
  if (s.state == SVC_NET_OFF) return -1;
  if (s.state != SVC_NET_CONNECTED) return 0;
  return s.rssi >= -60 ? 3 : (s.rssi >= -72 ? 2 : 1);
}

// Called by the service from any task after every state change / scan completion.
void onNetChange(bool scan_done) {
  svc_net_status_t s;
  svc_net_status(&s);
  os_ui_set_wifi(barLevel(s));
  Msg m{};
  m.topic = Topic::Network;
  m.app = 0;
  m.version = ++s_version;
  m.arg = (uint32_t)s.state | (scan_done ? 0x100u : 0u);
  bus().publish(m);
}

}  // namespace

Net* netService() { return &s_net; }

void netStart() {
  os_ui_set_wifi(-1);
  svc_net_init(onNetChange);
}

}  // namespace os::core
