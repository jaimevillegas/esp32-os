#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t vbat_mv;     // smoothed battery voltage
    uint8_t  percent;     // 0..100, from an open-circuit LiPo curve (inflated while charging)
    bool     usb;         // connected to a host computer (not a plain charger)
    bool     charging;    // inferred: no CHRG pin is wired to the MCU
    uint16_t vraw_mv;     // last measurement before filtering
    uint16_t vmin_mv;     // lowest / highest measurement of the last ~60 s
    uint16_t vmax_mv;
    uint16_t noise_mv;    // spread of the ADC samples inside one measurement
} os_power_state_t;

// Starts the `pwr` task, which samples every 2 s.
void os_power_start(void);
os_power_state_t os_power_get(void);

// Called from the `pwr` task when percent, usb or charging changed. Keep it short.
void os_power_set_change_cb(void (*cb)(const os_power_state_t *st));

#ifdef __cplusplus
}
#endif
