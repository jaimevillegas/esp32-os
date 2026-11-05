# Battery measurement findings (2026-11-04)

Board ES3C35P with a 3.7 V single-cell phone battery (capacity unknown). Question that started this
work: the battery percentage jumped around when the USB cable was removed, and later dropped 1 %
per second. This note records what was measured, what the hardware actually is, which conclusions
hold, and the test that is still pending.

## Hardware (from the vendor schematic, `5-Schematic/ESP32-S3 Schematic.pdf`)

- The battery is connected through `JP1` to the net `BAT+`. The same net feeds the charger output
  (`U2`, TP4054, pin `BAT`), the power path MOSFET `Q3` (SL2305, `BAT+` -> system `+5`) and the ADC
  divider `R14`/`R15` (100 kΩ / 100 kΩ). The ADC (IO8, ADC1_CH7) therefore reads the **battery
  terminal**, halved by the divider: `V_bat = 2 * V_adc`.
- Charge current is set by `R12` = 3.3 kΩ: I = 1000 / 3.3 ≈ **300 mA** (TP4054 formula).
- The charger status pin `CHRG` (U2 pin 1) only drives an LED through `R11`; it is **not wired to any
  GPIO**. `VBUS` is not wired to a GPIO either. "Charging" can only be inferred.
- The divider presents a 50 kΩ source impedance to the ADC (it prefers about 10 kΩ), so single
  conversions are noisy.
- USB attached to a host computer is detected with `usb_serial_jtag_is_connected()` (it needs SOF
  packets: a plain charger or power bank is **not** reported).

## Measurements

| Situation | Reading | Notes |
| --- | --- | --- |
| USB connected, charging | 4.146 - 4.152 V | spread of 0 - 4 mV inside a measurement, flat for minutes |
| After unplugging, board awake (screen on) | 3.76 V (range 3.76 - 4.15 V) | percentage walked from 84 % down to 21 % and stopped |
| After 2 min of light sleep, then wake | 3.75 V (range 3.73 - 3.77 V) | **not a valid rest measurement**, see below |
| USB reconnected | 4.13 V | |

### The 1 %/s fall was a filter artifact, not a discharge

The old filter averaged the voltage slowly (about 16 s time constant) and, while discharging, let the
percentage fall by one point per sample. After the cable was removed the average still remembered the
~4.15 V of the charger and walked down to the value of the real loaded voltage (3.74 V = 21 %), where
it stopped. A 3000 mAh cell at ~200 mA loses 1 % in about 9 minutes, so 1 %/s cannot be real.

Fixes (in `components/os_power` and `components/bsp_es3c35p/bsp_power.c`):

- 64 ADC conversions per measurement, trimmed mean (the middle half), instead of a median of 8.
- On every USB plug/unplug the average, the trend history and the displayed percentage restart from
  the present measurement.
- Hysteresis (30 mV) on the "battery full" threshold, so the charging icon no longer flickers.
- While discharging the percentage can only fall, at most one point per sample, and rises only on
  clear evidence; without a USB host "charging" is inferred only from a rise of at least 80 mV in 30 s.
- Diagnostics shows the minimum and maximum measured voltage of the last minute next to the state
  (`Bat 3.88-3.95V`), which exposes noise and load sag.

## What the data says about the battery

Voltage at the shared `BAT+` node = open-circuit voltage + I * R_internal, with I positive while
charging. Using the two valid points (charging at about +300 mA reads 4.13 V; discharging at about
-200 mA reads 3.76 V): 0.37 V / 0.5 A = **about 0.75 Ω**, high for a phone cell (a healthy one is
0.1 - 0.2 Ω). If that is right, the resting voltage would be about 3.9 V, that is **55 - 60 %**, and the
21 % shown from the loaded voltage is pessimistic. This is an estimate, not a result.

### The light-sleep "rest" test was invalid

Light sleep halts the CPU, so no ADC measurements are taken while asleep, and on wake the screen and the
rest of the load come back at once. The 3.75 V read afterwards was taken under load, not at rest. It must
not be used as the open-circuit voltage.

## Pending test: brightness as a variable load

The backlight is the largest consumer, so it can be used as a known step in load. Procedure, USB
**disconnected**, Diagnostics open to read the voltage:

1. Set the brightness to the minimum (5 %), wait 1 minute without touching anything, note the voltage.
2. Set the brightness to 100 %, wait 1 minute, note the voltage.

Difference in current between both settings is over 100 mA. Decision rule:

| Voltage drop 5 % -> 100 % | Meaning | Action |
| --- | --- | --- |
| 80 mV or more | high internal resistance; the percentage from the loaded voltage is pessimistic | estimate the load from brightness (and active radios/audio) and add `I * R` to the voltage before the curve lookup |
| 10 - 20 mV | normal resistance; 21 % is right | investigate why the charging reading is so high (charger behaviour, battery protection board) |

Results of this test: **not recorded yet** (fill in below).

| Brightness | Voltage | Range (1 min) |
| --- | --- | --- |
| 5 % | | |
| 100 % | | |

## Related findings from the same session

- The touch controller reports one extra "touched" sample (about 16 ms, same position) 17 - 33 ms
  after a finger lifts, which turned every tap into a double click. The indev callback now needs two
  consecutive touched reads to report a press and ignores new presses for 60 ms after a release.
  Measured afterwards: no re-press within 100 ms of a release in 39 taps.
- `lv_keyboard_create()` aligns the keyboard to the bottom; a later `lv_obj_set_pos` is added to that
  alignment and pushed the keyboard 88 px out of the visible area (only two rows showed). Use
  `lv_obj_align(..., LV_ALIGN_TOP_LEFT, x, y)` instead.
- A one-line `lv_textarea` whose height is smaller than the line height plus the default padding scrolls
  vertically; reduce the vertical padding and allow horizontal scrolling only.
