file(GLOB_RECURSE srcs "*.c" "*.cpp")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_driver_i2s esp_driver_i2c esp_driver_gpio
                                     esp_timer bsp_es3c35p)
