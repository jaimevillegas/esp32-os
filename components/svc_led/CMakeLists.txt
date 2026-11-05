file(GLOB_RECURSE srcs "*.c" "*.cpp")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_rmt esp_driver_gpio esp_rom)
