/*
 * GT911 touch wrapper - ported from Arduino C++ to ESP-IDF C
 */
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "gt911_touch.h"

#define CONFIG_LCD_HRES 1024
#define CONFIG_LCD_VRES 600

static const char *TAG = "gt911_touch";

static esp_lcd_touch_handle_t s_tp = NULL;
static esp_lcd_panel_io_handle_t s_tp_io_handle = NULL;

esp_err_t gt911_touch_init(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin)
{
    /* The system and legacy launcher both call this initializer.  Keep one
     * valid handle and never expose the driver's freed error-path object. */
    if (s_tp) {
        ESP_LOGI(TAG, "GT911 already initialized");
        return ESP_OK;
    }

    // Get the I2C bus handle (bus must be initialized beforehand)
    i2c_master_bus_handle_t i2c_handle = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(0, &i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus unavailable for GT911: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = CONFIG_LCD_HRES,
        .y_max = CONFIG_LCD_VRES,
        .rst_gpio_num = (gpio_num_t)rst_pin,
        .int_gpio_num = (gpio_num_t)int_pin,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    /* GT911 selects its address from the INT strap during reset. The target
     * board documentation permits either address, so probe both. */
    const uint8_t addresses[] = { 0x5D, 0x14 };
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_config.dev_addr = addresses[i];
        tp_io_config.scl_speed_hz = 100000;
        ESP_LOGI(TAG, "Initialize touch IO (I2C address 0x%02x)", addresses[i]);
        ret = esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &s_tp_io_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GT911 I2C panel IO init failed: %s", esp_err_to_name(ret));
            s_tp_io_handle = NULL;
            continue;
        }

        ESP_LOGI(TAG, "Initialize touch controller GT911");
        ret = esp_lcd_touch_new_i2c_gt911(s_tp_io_handle, &tp_cfg, &s_tp);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 detected at I2C address 0x%02x", addresses[i]);
            return ESP_OK;
        }
        s_tp = NULL;
    }

    ESP_LOGW(TAG, "GT911 not detected; continuing without touch (%s)",
             esp_err_to_name(ret));
    return ret;
}

bool gt911_touch_get_xy(uint16_t *x, uint16_t *y)
{
    if (!s_tp) return false;

    uint16_t touch_strength[1];
    uint8_t touch_cnt = 0;

    esp_lcd_touch_read_data(s_tp);
    bool touched = esp_lcd_touch_get_coordinates(s_tp, x, y, touch_strength, &touch_cnt, 1);

    return touched;
}
