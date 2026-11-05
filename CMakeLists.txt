cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(es3c35p-os)

# The firmware is built for size (-Os) because the IRAM code of the radio stacks eats internal
# SRAM; the components that are performance-critical keep -O2.
foreach(comp lvgl__lvgl chmorgan__esp-libhelix-mp3 os_ui)
  idf_component_get_property(lib ${comp} COMPONENT_LIB)
  if(lib)
    target_compile_options(${lib} PRIVATE -O2)
  endif()
endforeach()
