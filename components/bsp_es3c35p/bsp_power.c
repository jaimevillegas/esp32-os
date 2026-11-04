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

uint32_t bsp_battery_read_mv(void)
{
    int mv[SAMPLES];
    int n = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int raw, v;
        if (adc_oneshot_read(s_adc, BAT_CHANNEL, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &v) == ESP_OK) {
            mv[n++] = v;
        }
    }
    if (n == 0) return 0;
    for (int i = 1; i < n; i++) {               // insertion sort, n <= 8
        int k = mv[i], j = i - 1;
        while (j >= 0 && mv[j] > k) { mv[j + 1] = mv[j]; j--; }
        mv[j + 1] = k;
    }
    return (uint32_t)mv[n / 2] * BSP_BAT_DIVIDER;
}

bool bsp_usb_host_connected(void)
{
    return usb_serial_jtag_is_connected();
}
