# ============================================================
# peripheral_uart — top-level build Makefile
# ============================================================
# Usage:
#   make              — build → build/merged.hex
#   make flash        — flash via west
#   make clean        — remove build/
#   make pristine     — clean + build
#
#   Override board or toolchain at the command line:
#     make BOARD=nrf52840dk/nrf52840
#     make NCS_DIR=/opt/nordic/ncs/v2.6.0
# ============================================================

APP_DIR      := $(CURDIR)
ROOT_DIR     := $(CURDIR)
BUILD_DIR    := $(ROOT_DIR)/build

BOARD         ?= nrf52dk/nrf52832
NCS_DIR       ?= /opt/nordic/ncs/v3.0.1
TOOLCHAIN_DIR ?= /opt/nordic/ncs/toolchains/ef4fc6722e

LOCAL_HOME    := $(ROOT_DIR)/.home
WEST          := $(TOOLCHAIN_DIR)/bin/west

# ----------------------------------------------------------
# Build timestamp — captured once when make is invoked.
# expr strips leading zeros so the values are plain decimals
# (avoids octal literal issues in C: 08 / 09 are invalid).
# ----------------------------------------------------------
BUILD_YEAR  := $(shell date "+%Y")
BUILD_MONTH := $(shell expr `date "+%m"` + 0)
BUILD_DAY   := $(shell expr `date "+%d"` + 0)
BUILD_HOUR  := $(shell expr `date "+%H"` + 0)
BUILD_MIN   := $(shell expr `date "+%M"` + 0)
BUILD_SEC   := $(shell expr `date "+%S"` + 0)

export HOME                     := $(LOCAL_HOME)
export ZEPHYR_BASE              := $(NCS_DIR)/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT := zephyr
export ZEPHYR_SDK_INSTALL_DIR   := $(TOOLCHAIN_DIR)/opt/zephyr-sdk
export PATH := $(TOOLCHAIN_DIR)/bin:$(TOOLCHAIN_DIR)/usr/bin:$(TOOLCHAIN_DIR)/usr/local/bin:$(TOOLCHAIN_DIR)/opt/bin:$(TOOLCHAIN_DIR)/opt/nanopb/generator-bin:$(TOOLCHAIN_DIR)/opt/zephyr-sdk/arm-zephyr-eabi/bin:$(PATH)

# CMake extra args — time injected into every build unit via zephyr_compile_definitions.
CMAKE_EXTRA := \
	-DAPP_BUILD_YEAR=$(BUILD_YEAR)   \
	-DAPP_BUILD_MONTH=$(BUILD_MONTH) \
	-DAPP_BUILD_DAY=$(BUILD_DAY)     \
	-DAPP_BUILD_HOUR=$(BUILD_HOUR)   \
	-DAPP_BUILD_MIN=$(BUILD_MIN)     \
	-DAPP_BUILD_SEC=$(BUILD_SEC)

.PHONY: build flash clean pristine

build:
	@echo ">>> Build timestamp: $(BUILD_YEAR)-$(BUILD_MONTH)-$(BUILD_DAY) $(BUILD_HOUR):$(BUILD_MIN):$(BUILD_SEC)"
	@mkdir -p "$(LOCAL_HOME)/Library/Caches/zephyr/ToolchainCapabilityDatabase"
	"$(WEST)" build -p always -b "$(BOARD)" "$(APP_DIR)" -d "$(BUILD_DIR)" \
		-- $(CMAKE_EXTRA)
	@echo ">>> HEX ready: $(BUILD_DIR)/merged.hex"

flash:
	"$(WEST)" flash -d "$(BUILD_DIR)"

clean:
	rm -rf "$(BUILD_DIR)"

pristine: clean build
