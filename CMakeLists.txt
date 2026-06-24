#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(peripheral_uart)

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

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
  src/drivers/soft_spi.c
  src/drivers/ssd1675a.c
  src/lib/eink_lut.c
  src/lib/graphics.c
  src/lib/dither.c
  src/lib/life.c
  src/app/battery.c
  src/app/display_manager.c
  src/app/display_screens.c
  src/app/commands.c
  src/app/system_time.c
  src/app/persist.c
  src/app/factory.c
  src/app/secauth.c
  src/ble/ble_service.c
)

zephyr_include_directories(src)


# NORDIC SDK APP END
