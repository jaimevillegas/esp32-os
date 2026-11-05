idf_component_register(SRCS "svc_net.cpp" INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer esp_hw_support)
