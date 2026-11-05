// The `svc` task: runs the Service of every background app cooperatively.
#include "core_internal.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace os::core {

static const char* TAG = "svc";
static const uint32_t kSlowMs = 20;

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static void svc_task(void*) {
  for (;;) {
    uint32_t now = nowMs();
    uint32_t wait = 1000;
    for (size_t i = 0; i < slotCount(); i++) {
      Slot& s = *slotAt(i);
      if (!s.svc) continue;
      if (s.tick_pending && (int32_t)(now - s.next_tick) >= 0) {
        const uint32_t t0 = nowMs();
        const uint32_t next = s.svc->onTick(t0);
        if (nowMs() - t0 > kSlowMs) ESP_LOGW(TAG, "%s onTick took %u ms", s.m->id, (unsigned)(nowMs() - t0));
        s.tick_pending = next != UINT32_MAX;
        s.next_tick = nowMs() + next;
      }
      if (s.tick_pending) {
        const int32_t d = (int32_t)(s.next_tick - nowMs());
        if (d > 0 && (uint32_t)d < wait) wait = (uint32_t)d;
        else if (d <= 0) wait = 0;
      }
    }
    Msg m;
    if (busReceiveSvc(m, wait)) {
      do {
        for (size_t i = 0; i < slotCount(); i++) {
          Slot& s = *slotAt(i);
          if (!s.svc || (m.app != 0 && m.app != s.id)) continue;
          const uint32_t t0 = nowMs();
          s.svc->onEvent(m);
          if (nowMs() - t0 > kSlowMs) ESP_LOGW(TAG, "%s onEvent took %u ms", s.m->id, (unsigned)(nowMs() - t0));
        }
      } while (busReceiveSvc(m, 0));
    }
  }
}

void startServices() {
  for (size_t i = 0; i < slotCount(); i++) {
    Slot& s = *slotAt(i);
    if (!s.m->background || !s.m->make_service || !s.enabled) continue;
    if (!ensureArena(s)) continue;
    s.svc = s.m->make_service(s.arena);
    s.svc->onCreate(*s.ctx);
    s.tick_pending = true;
    s.next_tick = nowMs();
    ESP_LOGI(TAG, "service %s started", s.m->id);
  }
  xTaskCreatePinnedToCore(svc_task, "svc", 6144, nullptr, 5, nullptr, 0);
}

}  // namespace os::core
