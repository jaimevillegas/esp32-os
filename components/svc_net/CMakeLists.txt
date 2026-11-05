file(GLOB_RECURSE srcs "*.c" "*.cpp")
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer)
