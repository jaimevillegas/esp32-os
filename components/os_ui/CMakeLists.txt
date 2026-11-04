idf_component_register(SRCS "display_bridge.c" "test_screen.c" "status_bar.c" "shade.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bsp_es3c35p os_power lvgl__lvgl)
