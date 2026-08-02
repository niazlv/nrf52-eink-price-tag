cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eink_demo)

set(EINK_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

# The same portable library the main firmware uses — nothing is copied here.
target_sources(app PRIVATE
  ${EINK_LIB_DIR}/eink/ssd1675a.c
  ${EINK_LIB_DIR}/eink/eink_lut.c
  ${EINK_LIB_DIR}/eink/port/ssd1675a_port_zephyr_nrf.c
  ${EINK_LIB_DIR}/gfx/graphics.c
)

target_sources(app PRIVATE
  src/main.c
  src/mandelbrot.c
)

zephyr_include_directories(src ${EINK_LIB_DIR})
