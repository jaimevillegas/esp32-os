idf_component_register(SRCS "arena.cpp" "bus.cpp" "kv.cpp" "device.cpp" "registry.cpp"
                            "app_manager.cpp" "launcher.cpp" "svc.cpp" "os_core.cpp"
                            "storage_glue.cpp" "audio_glue.cpp" "sleep_glue.cpp"
                            "net_glue.cpp" "ble_glue.cpp" "led_glue.cpp" "recorder_glue.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES os_api os_ui os_power apps svc_storage svc_audio svc_net svc_ble svc_led bsp_es3c35p
                       PRIV_REQUIRES nvs_flash heap esp_timer esp_hw_support esp_pm driver
                                    esp_driver_tsens bootloader_support esp_partition app_update spi_flash)
