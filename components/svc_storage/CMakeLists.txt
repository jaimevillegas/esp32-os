file(GLOB_RECURSE srcs "*.c" "*.cpp")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer sdmmc fatfs vfs esp_driver_sdmmc)
