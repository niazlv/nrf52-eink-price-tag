#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eink_tag)

# Inject build timestamp as C preprocessor defines when passed from Makefile.
if(DEFINED APP_BUILD_YEAR)
  zephyr_compile_definitions(
    APP_BUILD_YEAR=${APP_BUILD_YEAR}
    APP_BUILD_MONTH=${APP_BUILD_MONTH}
    APP_BUILD_DAY=${APP_BUILD_DAY}
    APP_BUILD_HOUR=${APP_BUILD_HOUR}
    APP_BUILD_MIN=${APP_BUILD_MIN}
    APP_BUILD_SEC=${APP_BUILD_SEC}
  )
  message(STATUS "Build time: ${APP_BUILD_YEAR}-${APP_BUILD_MONTH}-${APP_BUILD_DAY} ${APP_BUILD_HOUR}:${APP_BUILD_MIN}:${APP_BUILD_SEC}")
endif()

# Portable display library (lib/) — no Zephyr dependency except the port file.
# See lib/README.md for reusing it outside this project.
target_sources(app PRIVATE
  lib/eink/ssd1675a.c
  lib/eink/eink_lut.c
  lib/eink/port/ssd1675a_port_zephyr_nrf.c
  lib/gfx/graphics.c
  lib/gfx/dither.c
  lib/gfx/life.c
)

# Application
target_sources(app PRIVATE
  src/main.c
  src/app/battery.c
  src/app/display_manager.c
  src/app/display_screens.c
  src/app/commands.c
  src/app/system_time.c
  src/app/persist.c
  src/app/factory.c
  src/app/secauth.c
  src/app/mesh.c
  src/ble/ble_service.c
)

zephyr_include_directories(src lib)
