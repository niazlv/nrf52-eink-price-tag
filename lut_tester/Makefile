APP_DIR  := $(CURDIR)
ROOT_DIR := $(abspath $(APP_DIR)/..)
BUILD_DIR := $(ROOT_DIR)/build_lut_tester

BOARD     ?= nrf52dk/nrf52832
NCS_DIR   ?= /opt/nordic/ncs/v3.0.1
TOOLCHAIN_DIR ?= /opt/nordic/ncs/toolchains/ef4fc6722e

LOCAL_HOME := $(ROOT_DIR)/.home
WEST := $(TOOLCHAIN_DIR)/bin/west

export HOME                    := $(LOCAL_HOME)
export ZEPHYR_BASE             := $(NCS_DIR)/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT := zephyr
export ZEPHYR_SDK_INSTALL_DIR  := $(TOOLCHAIN_DIR)/opt/zephyr-sdk
export PATH := $(TOOLCHAIN_DIR)/bin:$(TOOLCHAIN_DIR)/usr/bin:$(TOOLCHAIN_DIR)/usr/local/bin:$(TOOLCHAIN_DIR)/opt/bin:$(TOOLCHAIN_DIR)/opt/nanopb/generator-bin:$(TOOLCHAIN_DIR)/opt/zephyr-sdk/arm-zephyr-eabi/bin:$(PATH)

.PHONY: build clean flash pristine

build:
	mkdir -p "$(LOCAL_HOME)/Library/Caches/zephyr/ToolchainCapabilityDatabase"
	"$(WEST)" build -p always -b "$(BOARD)" "$(APP_DIR)" -d "$(BUILD_DIR)"
	cp "$(BUILD_DIR)/merged.hex" "$(BUILD_DIR)/lut_tester.hex"
	@echo "HEX: $(BUILD_DIR)/lut_tester.hex"

flash:
	"$(WEST)" flash -d "$(BUILD_DIR)"

clean:
	rm -rf "$(BUILD_DIR)"

pristine: clean build
