idf_component_register(SRCS "bsp_display.c" "bsp_i2c.c" "bsp_touch.c" "bsp_power.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd driver esp_timer esp_adc esp_driver_usb_serial_jtag
                       PRIV_REQUIRES espressif__esp_lcd_st77922)
