# Host unit tests for the node's COBS layer. No ESP-IDF, no framework.
# Build:  cmake -S test/host -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir> --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(car_hvac_node_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()


set(MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../../main)
add_compile_options(-Wall -Wextra)

# cobs_protocol.c is compiled with the pad switch as LINK_PAD_TRAILING_ZERO=1 (node_config.h default)
# and =0 (padding compiled out); the source honours a -D override instead of including node_config.h.
foreach(pad 1 0)
  add_executable(test_cobs_node_pad${pad} test_cobs_node.c ${MAIN}/cobs_protocol.c)
  target_include_directories(test_cobs_node_pad${pad} PRIVATE ${MAIN} .)
  target_compile_definitions(test_cobs_node_pad${pad} PRIVATE LINK_PAD_TRAILING_ZERO=${pad})
  add_test(NAME test_cobs_node_pad${pad} COMMAND test_cobs_node_pad${pad})
endforeach()
