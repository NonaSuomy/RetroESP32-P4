#include "elecrow_esp32_p4_aio.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "elecrow_aio";
enum { INPUT_UP = 0, INPUT_RIGHT, INPUT_DOWN, INPUT_LEFT,
       INPUT_SELECT, INPUT_START, INPUT_A };
static adc_oneshot_unit_handle_t s_adc = NULL;
static int s_last_region = -1;
static int s_touch_last = 0;
static bool s_touch_seen_low = false;

void elecrow_esp32_p4_aio_init(void)
{
    gpio_config_t touch = {
        .pin_bit_mask = 1ULL << 2,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&touch));

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &chan_cfg));
    ESP_LOGI(TAG, "Elecrow ESP32-P4 AIO controller ready (ADC GPIO16, touch GPIO2)");
}

void elecrow_esp32_p4_aio_read(int *values, int value_count)
{
    if (!values || value_count < 7 || !s_adc) return;

    /* The ladder sits near the ADC rail when released. A single conversion
     * can briefly dip into a button band and create phantom navigation. This
     * median matches the filtering used by the board's ESPHome reference. */
    int samples[5];
    for (int i = 0; i < 5; ++i) {
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &samples[i]) != ESP_OK) return;
    }
    for (int i = 1; i < 5; ++i) {
        int v = samples[i], j = i - 1;
        while (j >= 0 && samples[j] > v) {
            samples[j + 1] = samples[j];
            --j;
        }
        samples[j + 1] = v;
    }
    int adc = samples[2];
    int region = adc >= 3350 ? 0 : adc;
    if (region != s_last_region) {
        ESP_LOGI(TAG, "controls adc=%d touch=%d", adc, gpio_get_level(2));
        s_last_region = region;
    }

    if (adc < 3350) {
        /* ESPHome bands converted to 12-bit ADC raw values (3.861 V full
         * scale): 1.48, 1.60, 1.75, 1.95, 2.10, 2.40, 2.75, 3.10 V. */
        if (adc < 1570) { values[INPUT_DOWN] = 1; values[INPUT_LEFT] = 1; }
        else if (adc < 1696) { values[INPUT_UP] = 1; values[INPUT_LEFT] = 1; }
        else if (adc < 1855) values[INPUT_LEFT] = 1;
        else if (adc < 2068) { values[INPUT_DOWN] = 1; values[INPUT_RIGHT] = 1; }
        else if (adc < 2226) { values[INPUT_UP] = 1; values[INPUT_RIGHT] = 1; }
        else if (adc < 2545) values[INPUT_RIGHT] = 1;
        else if (adc < 2916) values[INPUT_DOWN] = 1;
        else if (adc < 3287) values[INPUT_UP] = 1;
    }

    /* GPIO2 is the active-high TTP223 output. Some boards power up with the
     * line high, so ignore that initial state until we have observed an
     * actual released (low) state. Only the rising edge is a button press;
     * treating the falling edge as A caused a phantom press on release. */
    int touch = gpio_get_level(2);
    if (!s_touch_seen_low) {
        if (!touch) s_touch_seen_low = true;
        s_touch_last = touch;
    } else if (touch && !s_touch_last) {
        values[INPUT_A] = 1;
        ESP_LOGI(TAG, "touch event gpio2=%d", touch);
        s_touch_last = touch;
    } else {
        s_touch_last = touch;
    }
}
