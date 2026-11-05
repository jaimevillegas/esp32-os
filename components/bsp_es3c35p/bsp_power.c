#include "bsp_es3c35p.h"

#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bsp_power";

#define BAT_CHANNEL ADC_CHANNEL_7   // GPIO8 on ADC1
#define SAMPLES     8

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

esp_err_t bsp_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "adc unit");
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BAT_CHANNEL, &ch), TAG, "adc channel");
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .chan = BAT_CHANNEL,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cal, &s_cali), TAG, "adc calibration");
    return ESP_OK;
}

// The divider (100k/100k) gives the ADC a 50k source impedance, far above the ~10k it likes, so single
// conversions are noisy. Take many, sort, drop the extreme quarter on each side and average the
// rest. `noise_mv` is the spread of the kept samples (battery side), a quality indicator.
#define BAT_SAMPLES 64

uint32_t bsp_battery_read_stats(uint32_t *noise_mv)
{
    int raw[BAT_SAMPLES];
    int n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int r;
        if (adc_oneshot_read(s_adc, BAT_CHANNEL, &r) == ESP_OK) raw[n++] = r;
    }
    if (n < 8) {
        if (noise_mv) *noise_mv = 0;
        return 0;
    }
    for (int i = 1; i < n; i++) {               // insertion sort
        int k = raw[i], j = i - 1;
        while (j >= 0 && raw[j] > k) { raw[j + 1] = raw[j]; j--; }
        raw[j + 1] = k;
    }
    const int lo = n / 4, hi = n - n / 4;       // keep the middle half
    long sum = 0;
    for (int i = lo; i < hi; i++) sum += raw[i];
    const int avg = (int)(sum / (hi - lo));
    int v = 0, vlo = 0, vhi = 0;
    adc_cali_raw_to_voltage(s_cali, avg, &v);
    adc_cali_raw_to_voltage(s_cali, raw[lo], &vlo);
    adc_cali_raw_to_voltage(s_cali, raw[hi - 1], &vhi);
    if (noise_mv) *noise_mv = (uint32_t)(vhi - vlo) * BSP_BAT_DIVIDER;
    return (uint32_t)v * BSP_BAT_DIVIDER;
}

uint32_t bsp_battery_read_mv(void) { return bsp_battery_read_stats(NULL); }

bool bsp_usb_host_connected(void)
{
    return usb_serial_jtag_is_connected();
}
