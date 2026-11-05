#include "os_power.h"

#include "bsp_es3c35p.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "os_power";

#define PERIOD_MS      2000
#define TREND_SAMPLES  15           // 30 s window
#define TREND_MV       15           // rise/fall that counts as charging / discharging
#define FULL_MV        4150

// Typical single-cell LiPo open-circuit curve (descending voltage).
static const struct { uint16_t mv; uint8_t pct; } k_curve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
    {3910, 65}, {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
    {3770, 30}, {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5}, {3270, 0},
};

static os_power_state_t s_state;
static void (*s_change_cb)(const os_power_state_t *);

void os_power_set_change_cb(void (*cb)(const os_power_state_t *)) { s_change_cb = cb; }
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t mv_to_percent(uint32_t mv)
{
    const int n = sizeof k_curve / sizeof k_curve[0];
    if (mv >= k_curve[0].mv) return 100;
    if (mv <= k_curve[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= k_curve[i].mv) {
            uint32_t span = k_curve[i - 1].mv - k_curve[i].mv;
            uint32_t pct = k_curve[i].pct + (mv - k_curve[i].mv) * (k_curve[i - 1].pct - k_curve[i].pct) / span;
            return (uint8_t)pct;
        }
    }
    return 0;
}

os_power_state_t os_power_get(void)
{
    taskENTER_CRITICAL(&s_lock);
    os_power_state_t copy = s_state;
    taskEXIT_CRITICAL(&s_lock);
    return copy;
}

/* Filtering rules (the raw ADC value is noisy and the battery sags under load):
 *  - measurement: trimmed mean of 64 conversions (bsp_battery_read_stats);
 *  - voltage: exponential average, ~16 s time constant while on battery, ~8 s while charging;
 *  - while discharging the displayed percentage can only fall, at most 1 point per sample, and
 *    only rises again when the filtered voltage clearly exceeds it (3+ points) - a rise is then
 *    followed at 1 point per sample;
 *  - when charging stops the percentage is re-synchronised at once;
 *  - without a USB host (plain charger or battery) charging is inferred only from a large and
 *    sustained rise (>= 80 mV in 30 s), so ADC noise and load transients cannot fake it. */
#define RANGE_SAMPLES   30          /* 60 s min/max window */
#define NO_HOST_RISE_MV 80

static void pwr_task(void *arg)
{
    uint32_t hist[TREND_SAMPLES] = {0};
    uint16_t raws[RANGE_SAMPLES] = {0};
    int idx = 0, filled = 0, ridx = 0, rfilled = 0, log_cnt = 0;
    float ema = 0;
    int shown = -1;                  /* displayed percentage, -1 until the first sample */
    bool was_charging = false, was_usb = false, full = false;
    for (;;) {
        uint32_t noise = 0;
        const uint32_t mv = bsp_battery_read_stats(&noise);
        if (mv) {
            const bool usb = bsp_usb_host_connected();
            const bool first = ema == 0;
            if (!first && usb != was_usb) {
                /* Plug / unplug: the supply conditions changed at once (charge voltage vs the
                 * loaded battery), so the slow average and the trend history are meaningless:
                 * restart them from the present measurement. */
                ema = (float)mv;
                filled = 0;
                idx = 0;
                shown = -1;
            }
            was_usb = usb;
            const float alpha = was_charging ? 1.0f / 4.0f : 1.0f / 8.0f;
            ema = first ? (float)mv : ema + ((float)mv - ema) * alpha;
            const uint32_t smooth = (uint32_t)ema;

            raws[ridx] = (uint16_t)mv;
            ridx = (ridx + 1) % RANGE_SAMPLES;
            if (rfilled < RANGE_SAMPLES) rfilled++;
            uint16_t vmin = 0xFFFF, vmax = 0;
            for (int i = 0; i < rfilled; i++) {
                if (raws[i] < vmin) vmin = raws[i];
                if (raws[i] > vmax) vmax = raws[i];
            }

            int32_t delta = 0;
            const bool have_trend = filled >= TREND_SAMPLES;
            if (have_trend) delta = (int32_t)smooth - (int32_t)hist[idx];
            hist[idx] = smooth;
            idx = (idx + 1) % TREND_SAMPLES;
            if (filled < TREND_SAMPLES) filled++;

            bool charging;
            if (usb) {
                /* A host is attached: assume charging unless the cell is full or clearly falling. */
                if (smooth >= FULL_MV) full = true;            /* hysteresis: avoids flicker at */
                else if (smooth < FULL_MV - 30) full = false;  /* the "battery full" threshold  */
                charging = !full && !(have_trend && delta <= -TREND_MV);
            } else {
                charging = have_trend && delta >= NO_HOST_RISE_MV;
            }

            const int target = mv_to_percent(smooth);
            if (shown < 0 || (was_charging && !charging)) {
                shown = target;                         /* start / end of a charge: resync */
            } else if (charging) {
                shown = target;                         /* the charge level may move both ways */
            } else if (target < shown) {
                shown -= 1;                             /* discharging: fall slowly... */
                if (shown < target) shown = target;
            } else if (target >= shown + 3) {
                shown += 1;                             /* ...rise only on clear evidence */
            }
            was_charging = charging;

            os_power_state_t st = {
                .vbat_mv = (uint16_t)smooth, .percent = (uint8_t)shown,
                .usb = usb, .charging = charging,
                .vraw_mv = (uint16_t)mv, .vmin_mv = vmin, .vmax_mv = vmax,
                .noise_mv = (uint16_t)noise,
            };
            taskENTER_CRITICAL(&s_lock);
            const os_power_state_t prev = s_state;
            s_state = st;
            taskEXIT_CRITICAL(&s_lock);
            if (s_change_cb && (prev.percent != st.percent || prev.usb != st.usb ||
                                prev.charging != st.charging)) {
                s_change_cb(&st);
            }

            if (log_cnt++ % 5 == 0) {
                ESP_LOGI(TAG, "vbat=%u (raw %u, %u..%u, noise %u) mV %u%% usb=%d charging=%d delta30s=%d",
                         st.vbat_mv, st.vraw_mv, st.vmin_mv, st.vmax_mv, st.noise_mv, st.percent,
                         st.usb, st.charging, (int)delta);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}

void os_power_start(void)
{
    ESP_ERROR_CHECK(bsp_battery_init());
    xTaskCreatePinnedToCore(pwr_task, "pwr", 3072, NULL, 2, NULL, 0);
}
