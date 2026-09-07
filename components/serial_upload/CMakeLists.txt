idf_component_register(
    SRCS "serial_upload.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_usb_serial_jtag esp_driver_uart vfs fatfs app_update esp_partition odroid
)
