# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL's 128 KB object pool goes to PSRAM instead of internal RAM, which Wi-Fi needs.
# LVGL 8 has no Kconfig entry for this, only lv_conf macros, and CMake cannot pass a
# function-style macro: plain malloc of a block above SPIRAM_MALLOC_ALWAYSINTERNAL
# (16 KB) is placed in PSRAM by SPIRAM_USE_MALLOC.
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_POOL_INCLUDE=<stdlib.h>" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_POOL_ALLOC=malloc" APPEND)
project(CarHvacController)
