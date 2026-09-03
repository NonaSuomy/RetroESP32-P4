idf_component_register(
    SRCS "st7701_lcd.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_ledc esp_lcd esp_lcd_ek79007 esp_hw_support ppa_engine
    PRIV_REQUIRES esp_mm
)
