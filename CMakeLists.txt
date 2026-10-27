cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(rds_decoder C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(rds_decoder
    src/main.c src/rds_dsp.c src/rds_sync.c src/rds_parser.c)

target_compile_options(rds_decoder PRIVATE -Wall -Wextra -Wdouble-promotion)
target_link_libraries(rds_decoder pico_stdlib pico_multicore pico_util
                      hardware_adc hardware_dma hardware_irq)

pico_set_binary_type(rds_decoder copy_to_ram)   # Code im RAM: kein Flash-Cache-Jitter
pico_enable_stdio_usb(rds_decoder 1)            # serielle Konsole ueber USB (CDC)
pico_enable_stdio_uart(rds_decoder 0)
pico_add_extra_outputs(rds_decoder)
