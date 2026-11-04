file(GLOB_RECURSE srcs "*.c" "*.cpp")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_i2s esp_timer bsp_es3c35p)
