#include "core_internal.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "os_ui.h"

namespace os::core {

static QueueHandle_t s_ui_q;
static QueueHandle_t s_svc_q;
static uint32_t s_dropped;

class BusImpl final : public Bus {
 public:
  bool publish(const Msg& in) override {
    Msg m = in;
    m.ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool ok = xQueueSend(s_ui_q, &m, 0) == pdTRUE;
    ok &= xQueueSend(s_svc_q, &m, 0) == pdTRUE;
    if (!ok) s_dropped++;
    os_ui_wake();
    return ok;
  }
};

static BusImpl s_bus;

void busInit() {
  s_ui_q = xQueueCreate(16, sizeof(Msg));
  s_svc_q = xQueueCreate(16, sizeof(Msg));
}

Bus& bus() { return s_bus; }

void busPumpUi() {
  Msg m;
  while (xQueueReceive(s_ui_q, &m, 0) == pdTRUE) {
    Slot* fg = foreground();
    if (fg && fg->app && (m.app == 0 || m.app == fg->id)) fg->app->onEvent(m);
  }
}

bool busReceiveSvc(Msg& m, uint32_t timeout_ms) {
  return xQueueReceive(s_svc_q, &m, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

}  // namespace os::core
