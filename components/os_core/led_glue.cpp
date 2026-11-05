// Glue between the RGB LED driver (svc_led) and the framework.
#include "core_internal.h"
#include "svc_led.h"

namespace os::core {

namespace {
class LedImpl final : public Led {
 public:
  void set(uint8_t r, uint8_t g, uint8_t b) override { svc_led_set(r, g, b); }
  void off() override { svc_led_set(0, 0, 0); }
};
LedImpl s_led;
bool s_ok = false;
}  // namespace

Led* ledService() { return s_ok ? &s_led : nullptr; }

// Called once before the UI starts: the LED is switched off right away.
void ledStart() { s_ok = svc_led_init(); }

}  // namespace os::core
