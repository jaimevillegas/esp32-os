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

static void pwr_task(void *arg)
{
    uint32_t hist[TREND_SAMPLES] = {0};
    int idx = 0, filled = 0, log_cnt = 0;
    float ema = 0;
    for (;;) {
        uint32_t mv = bsp_battery_read_mv();
        if (mv) {
            ema = (filled == 0 && ema == 0) ? (float)mv : ema + ((float)mv - ema) / 4.0f;
            const uint32_t smooth = (uint32_t)ema;
            const bool usb = bsp_usb_host_connected();

            // Charging is inferred ([MEASURE] on hardware): a rising voltage means charging, a
            // falling one means not; with no trend, assume charging while a host is connected
            // and the cell is not full.
            int32_t delta = 0;
            bool have_trend = filled >= TREND_SAMPLES;
            if (have_trend) delta = (int32_t)smooth - (int32_t)hist[idx];
            hist[idx] = smooth;
            idx = (idx + 1) % TREND_SAMPLES;
            if (filled < TREND_SAMPLES) filled++;

            bool charging;
            if (have_trend && delta >= TREND_MV)       charging = true;
            else if (have_trend && delta <= -TREND_MV) charging = false;
            else                                       charging = usb && smooth < FULL_MV;

            os_power_state_t st = {
                .vbat_mv = (uint16_t)smooth, .percent = mv_to_percent(smooth),
                .usb = usb, .charging = charging,
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
                ESP_LOGI(TAG, "vbat=%u mV (raw %u) %u%% usb=%d charging=%d delta30s=%d",
                         st.vbat_mv, (unsigned)mv, st.percent, st.usb, st.charging, (int)delta);
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
