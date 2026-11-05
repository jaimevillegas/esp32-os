file(GLOB_RECURSE srcs "*.c" "*.cpp")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt nvs_flash esp_timer)
