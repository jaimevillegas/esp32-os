# The service is compiled only when the BLE stack is enabled (see sdkconfig.defaults.ble).
if(CONFIG_BT_ENABLED)
  file(GLOB_RECURSE srcs "*.c" "*.cpp")
else()
  set(srcs "")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt nvs_flash esp_timer)
