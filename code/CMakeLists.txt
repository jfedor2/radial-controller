cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(radial)

pico_sdk_init()

add_compile_options(-Wall)

add_executable(radial src/radial.c)

target_include_directories(radial PRIVATE src)

target_link_libraries(radial pico_stdlib hardware_adc tinyusb_device tinyusb_board)

pico_add_extra_outputs(radial)
