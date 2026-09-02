#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eink_tag)

# Build timestamp, taken here rather than passed in.
#
# It used to come from the Makefile as -DAPP_BUILD_YEAR=... after `--`, but
# every -D there goes to the SYSBUILD cache and never reaches this image, so
# the block below was simply never entered: SYSINFO printed no build= field
# (the web showed "Сборка —") and system_time_init() fell back to
# __DATE__/__TIME__, which is per translation unit and only right on a
# pristine build. Reading the clock here removes the plumbing and the trap
# both — CMake re-runs on every pristine build, which is what `make build`
# does. A value passed in still wins, so the old override keeps working.
if(NOT DEFINED APP_BUILD_YEAR)
  string(TIMESTAMP APP_BUILD_YEAR  "%Y")
  string(TIMESTAMP APP_BUILD_MONTH "%m")
  string(TIMESTAMP APP_BUILD_DAY   "%d")
  string(TIMESTAMP APP_BUILD_HOUR  "%H")
  string(TIMESTAMP APP_BUILD_MIN   "%M")
  string(TIMESTAMP APP_BUILD_SEC   "%S")
  # Strip leading zeros: 08 and 09 are invalid octal literals in C.
  foreach(part APP_BUILD_MONTH APP_BUILD_DAY APP_BUILD_HOUR APP_BUILD_MIN APP_BUILD_SEC)
    math(EXPR ${part} "${${part}} + 0" OUTPUT_FORMAT DECIMAL)
  endforeach()
endif()

zephyr_compile_definitions(
  APP_BUILD_YEAR=${APP_BUILD_YEAR}
  APP_BUILD_MONTH=${APP_BUILD_MONTH}
  APP_BUILD_DAY=${APP_BUILD_DAY}
  APP_BUILD_HOUR=${APP_BUILD_HOUR}
  APP_BUILD_MIN=${APP_BUILD_MIN}
  APP_BUILD_SEC=${APP_BUILD_SEC}
)
message(STATUS "Build time: ${APP_BUILD_YEAR}-${APP_BUILD_MONTH}-${APP_BUILD_DAY} ${APP_BUILD_HOUR}:${APP_BUILD_MIN}:${APP_BUILD_SEC}")

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
  src/app/power_profile.c
  src/ble/ble_service.c
)

zephyr_include_directories(src lib)

# The image is built -Os (CONFIG_SIZE_OPTIMIZATIONS) from when flash was the
# binding constraint. It is not any more — ~32 KB spare there against ~96% of
# RAM used — so the two files that dominate per-frame CPU are worth building
# for speed. Costs a little flash, nothing in RAM.
#
# Deliberately NOT applied to lib/eink/port/ssd1675a_port_zephyr_nrf.c: its
# 9-bit bit-bang meets the panel's setup/hold with counted __NOP()s measured
# under -Os, and re-timing that needs a scope on real hardware, not a guess.
set_source_files_properties(
  lib/gfx/graphics.c
  lib/gfx/dither.c
  PROPERTIES COMPILE_OPTIONS "-O2")

# Frame-buffer arena, sized for the largest panel this image can drive: a
# 400x300 B/W plane is 15000 B; the 128x296 B/W/R default uses 9472 B of it.
# display_manager picks the layout at boot from the panel probe.
#
# Now a Kconfig value (CONFIG_EINK_FRAME_ARENA_BYTES) rather than a literal:
# an image built for a fleet with no 400x300 panels can set it to 9472 and get
# 5528 bytes of RAM back — on this build that is several times the remaining
# headroom. It has to go through Kconfig because a -D passed after `--` lands
# in the sysbuild cache, not in the application image.
target_compile_definitions(app PRIVATE
  GRAPHICS_ARENA_BYTES=${CONFIG_EINK_FRAME_ARENA_BYTES})
