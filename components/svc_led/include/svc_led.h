#pragma once
// RGB LED driver: one WS2812-class pixel on IO40, driven by the RMT peripheral.
// os_core/led_glue.cpp is the only user.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates the RMT channel and switches the LED off. Idempotent. Returns false on failure.
bool svc_led_init(void);
// Shows a colour immediately (0..255 per channel, logical RGB order). No-op before init.
void svc_led_set(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
